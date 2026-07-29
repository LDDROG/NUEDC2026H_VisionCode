# LDD_ROG 2026.7.29
# 钢球位置检测 + Modbus RTU 通信
# ============================================================
# 关键注意事项（务必读完再上手调试）：
#
# 1. 模型路径: 当前使用 yolo26s_ironball_to256x64.mud (256×64)，若换了模型务必同步改
#    MODEL_PATH 和 CAM_W/CAM_H。
# 2. 摄像头分辨率: 必须和模型训练分辨率一致，否则 YOLO 检测框会偏移。
# 3. UART 引脚: A16=TX, A17=RX 走 /dev/ttyS0，确认硬件上没有其他外设占用。
# 4. Modbus 从机地址: 默认 3，必须和 STM32H7 主站配置一致。
# 5. 中心点校准: PIPE_CENTER_X / PIPE_CENTER_Y 是水管中心在画面中的像素坐标，
#    摄像头安装好后必须实测标定一次，否则误差值完全不对。
# 6. 像素比例尺: PX_PER_CM 需要把水管 25cm 物理长度除以图中占的像素数，
#    不同安装高度对应不同值，换了结构就要重新标定。
# 7. 误差符号: 正值 = 球在中心右侧，负值 = 球在中心左侧。MCU 端 PID 方向要对应。
# 8. Modbus 轮询频率: STM32H7 主站轮询间隔建议 ≥10ms，太快 UART FIFO 可能溢出。
# 9. YOLO 模型预热: 加载后前 1~2 秒检测可能不稳定，比赛时提前上电。
# 10. RTSP 图传推流在另一台 MaixCam 上运行，与本脚本无硬件冲突。
# ============================================================

# 换模型改这些就行：MODEL_PATH, CAM_W/CAM_H, nn.YOLOxx(), print(), 标定参数



from maix import app, camera, display, image, nn, time
from maix import pinmap, uart
import math

# ======================== 视觉参数 ========================
CAM_W = 256
CAM_H = 64                       # ⚠️ 256×64 模型，必须和模型分辨率一致
MODEL_PATH = "/root/models/yolo26s_ironball_to256x64.mud"
CONF_TH = 0.4

# ======================== 水管标定参数 ========================
# ⚠️ 以下参数基于 256×64 分辨率估算，部署前必须实测标定！
# 实际取决于 MaixCam 驱动行为，可能偏差很大。
#
# 摄像头固定后，水管中心在画面中的像素坐标（必须实测标定！）
PIPE_CENTER_X = 128              # 256 的一半，水平中心
PIPE_CENTER_Y = 32               # 64 的一半，垂直中心
# 水管轴线与图像X轴的夹角（度），逆时针为正。
# 0°=水管水平，正值=水管右端上翘。
PIPE_ANGLE_DEG = 0.0             # 实际需要标定
# 像素/厘米比例尺 = 水管在画面中的像素长度 / 25cm
# 256px / 25cm ≈ 10.24，实际需用刻度线实测
PX_PER_CM = 10.24                # ⚠️ 必须实测标定！
# 水管在画面中的ROI区域（屏显参考框）
PIPE_ROI_X = 8
PIPE_ROI_Y = 4
PIPE_ROI_W = 240
PIPE_ROI_H = 56

# ======================== Modbus 配置 ========================
MODBUS_BAUD = 115200
MODBUS_SLAVE_ID = 3
# 寄存器地址分配
REG_BALL_X        = 0x0000   # 钢球中心X坐标（像素，0=未检测到）
REG_BALL_Y        = 0x0001   # 钢球中心Y坐标（像素，0=未检测到）
REG_ERROR_PX      = 0x0002   # 距中心点误差（像素，int16，有符号）
REG_CONFIDENCE    = 0x0003   # 置信度（0-1000，如 850 = 0.85）
REG_STATUS        = 0x0004   # 状态: 0=未检测到, 1=单球, 2=多球
REG_CENTER_X      = 0x0005   # 水管中心X（校准值，只读参考）
REG_SCALE         = 0x0006   # 像素/厘米 × 100（如 12.8 → 1280）
REG_FPS           = 0x0007   # 当前FPS × 10
HOLDING_REGS_COUNT = 16

# ======================== 预分配颜色 ========================
COLOR_GREEN  = image.Color.from_rgb(0, 255, 0)
COLOR_RED    = image.Color.from_rgb(255, 0, 0)
COLOR_YELLOW = image.Color.from_rgb(255, 255, 0)
COLOR_CYAN   = image.Color.from_rgb(0, 255, 255)


# ======================== CRC16 ========================
def _crc16(data):
    crc = 0xFFFF
    for b in data:
        crc ^= b
        for _ in range(8):
            if crc & 1:
                crc = (crc >> 1) ^ 0xA001
            else:
                crc >>= 1
    return crc


# ======================== Modbus 帧处理（裸UART，不依赖libmodbus） ========================
def _modbus_handle(u, holding, slave_id):
    """非阻塞读取UART，如果是完整Modbus帧就处理并回复"""
    raw = u.read(64)
    if not raw or len(raw) < 8:
        return
    if raw[0] != slave_id:
        return

    func = raw[1]
    if func == 0x03:   # Read Holding Registers
        start = (raw[2] << 8) | raw[3]
        count = (raw[4] << 8) | raw[5]
        resp = bytearray([slave_id, 0x03, count * 2])
        for i in range(count):
            v = holding[start + i] if (start + i) < len(holding) else 0
            resp.append((v >> 8) & 0xFF)
            resp.append(v & 0xFF)
        crc = _crc16(resp)
        resp.append(crc & 0xFF)
        resp.append((crc >> 8) & 0xFF)
        u.write(bytes(resp))

    elif func == 0x06:  # Write Single Register
        addr = (raw[2] << 8) | raw[3]
        val = (raw[4] << 8) | raw[5]
        if addr < len(holding):
            holding[addr] = val
        crc = _crc16(raw[:6])
        u.write(bytes(raw[:6]) + bytes([crc & 0xFF, (crc >> 8) & 0xFF]))


# ======================== 误差计算 ========================
def compute_error(ball_cx, ball_cy, center_x, center_y, angle_deg):
    """
    计算钢球沿水管方向的有符号误差（像素）。
    - 勾股距离 sqrt(dx²+dy²) = 沿水管方向的距离（因为球被约束在水管凹槽内）
    - 符号由投影到水管方向向量决定：
      正值 = 球在水管正方向一侧（右/上），负值 = 反方向一侧
    - angle_deg=0 时退化为：距离用勾股，符号看dx
    """
    dx = ball_cx - center_x
    dy = ball_cy - center_y
    dist = int(math.sqrt(dx * dx + dy * dy))

    if dist == 0:
        return 0

    rad = math.radians(angle_deg)
    # 水管单位方向向量
    pipe_dx = math.cos(rad)
    pipe_dy = math.sin(rad)
    # 投影到水管方向求符号
    proj = dx * pipe_dx + dy * pipe_dy
    return dist if proj >= 0 else -dist


# ======================== 绘制函数 ========================
def draw_detection(img, obj, center_x, center_y, error_px):
    """在图像上绘制检测结果"""
    x, y, bw, bh = obj.x, obj.y, obj.w, obj.h
    cx = x + bw // 2
    cy = y + bh // 2

    img.draw_rect(x, y, bw, bh, color=COLOR_GREEN, thickness=2)
    # img.draw_circle(cx, cy, 3, color=COLOR_RED, thickness=-1)
    # img.draw_line(center_x - 10, center_y, center_x + 10, center_y,
    #               color=COLOR_CYAN, thickness=1)
    # img.draw_line(center_x, center_y - 10, center_x, center_y + 10,
    #               color=COLOR_CYAN, thickness=1)
    # img.draw_string(cx + 8, cy - 16,
    #                 f"err={error_px:+d}px",
    #                 color=COLOR_YELLOW, scale=1.2)


# ======================== 主循环 ========================
def run():
    print("=" * 50)
    print("钢球平衡检测 — 2026H  [YOLO26s 256×64]")
    print(f"模型: {MODEL_PATH}")
    print(f"分辨率: {CAM_W}x{CAM_H}")
    print(f"Modbus从机地址: {MODBUS_SLAVE_ID}")
    print("=" * 50)

    # ---- UART初始化（先于摄像头，避免启动时序冲突） ----
    time.sleep(8)
    pinmap.set_pin_function("A16", "UART0_TX")
    pinmap.set_pin_function("A17", "UART0_RX")
    u = uart.UART("/dev/ttyS0", MODBUS_BAUD)
    holding_regs = [0] * HOLDING_REGS_COUNT
    print("Modbus RTU Slave (raw UART) ready")

    # ---- 摄像头 ----
    cam = camera.Camera(CAM_W, CAM_H)
    disp = display.Display()

    # ---- YOLO模型 ----
    print("加载YOLO模型...")
    detector = nn.YOLO26(model=MODEL_PATH, dual_buff=True)
    print(f"模型加载完成! 输入: {detector.input_width()}x{detector.input_height()}")

    # ---- 状态变量 ----
    frame_count = 0
    last_status_print = 0
    center_x = PIPE_CENTER_X
    center_y = PIPE_CENTER_Y

    # 将标定值写入寄存器（只读参考值）
    holding_regs[REG_CENTER_X] = PIPE_CENTER_X
    holding_regs[REG_SCALE] = int(PX_PER_CM * 100)

    last_ms = time.time_ms()

    while not app.need_exit():
        img = cam.read()
        if img is None:
            continue

        # ---- YOLO检测 ----
        objs = detector.detect(img, conf_th=CONF_TH)

        # ---- 提取钢球信息 ----
        ball_cx = 0
        ball_cy = 0
        confidence = 0
        status = 0
        error_px = 0

        if len(objs) == 1:
            obj = objs[0]
            ball_cx = obj.x + obj.w // 2
            ball_cy = obj.y + obj.h // 2
            confidence = int(obj.score * 1000)
            status = 1
            error_px = compute_error(ball_cx, ball_cy, center_x, center_y, PIPE_ANGLE_DEG)
            draw_detection(img, obj, center_x, center_y, error_px)
        elif len(objs) > 1:
            status = 2
            # 多球：取置信度最高的
            best = max(objs, key=lambda o: o.score)
            ball_cx = best.x + best.w // 2
            ball_cy = best.y + best.h // 2
            confidence = int(best.score * 1000)
            error_px = compute_error(ball_cx, ball_cy, center_x, center_y, PIPE_ANGLE_DEG)
            draw_detection(img, best, center_x, center_y, error_px)

        # ---- 更新Modbus寄存器 ----
        holding_regs[REG_BALL_X] = ball_cx
        holding_regs[REG_BALL_Y] = ball_cy
        holding_regs[REG_ERROR_PX] = error_px
        holding_regs[REG_CONFIDENCE] = confidence
        holding_regs[REG_STATUS] = status
        holding_regs[REG_CENTER_X] = center_x

        # ---- 调试输出 ----
        frame_count += 1
        if status != 0 and frame_count - last_status_print >= 30:
            print(f"[{frame_count:04d}] ball=({ball_cx},{ball_cy}) "
                  f"err={error_px:+d}px conf={confidence/1000:.2f} status={status}")
            last_status_print = frame_count

        # ---- FPS ----
        now = time.time_ms()
        fps = 1000.0 / max(now - last_ms, 1)
        last_ms = now
        holding_regs[REG_FPS] = int(fps * 10)
        if frame_count % 30 == 0:
            img.draw_string(0, 0, f"FPS:{fps:.1f}", color=COLOR_YELLOW, scale=1.2)
        # img.draw_string(0, 18, f"err:{error_px:+d}px",
        #                 color=COLOR_RED if abs(error_px) > int(PX_PER_CM) else COLOR_GREEN,
        #                 scale=1.2)

        # # 画出水管ROI参考线
        # img.draw_rect(PIPE_ROI_X, PIPE_ROI_Y, PIPE_ROI_W, PIPE_ROI_H,
        #               color=COLOR_CYAN, thickness=1)

        # ---- Modbus响应 ----
        _modbus_handle(u, holding_regs, MODBUS_SLAVE_ID)

        disp.show(img)