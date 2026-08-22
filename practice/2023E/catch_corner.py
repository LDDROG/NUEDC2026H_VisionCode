# LDD_ROG 2026.7.13
# 矩形效果很离谱的话，优先检查前面几个参数、二值化反向问题、矩形跳变过滤阈值
# 浮点运算很吃性能！！！

# 视觉库
from maix import app, camera, display, image, time
import cv2
import numpy as np
# 串口库
from maix import pinmap, uart

CAM_W = 512    # 定义画面宽
CAM_H = 320    # 定义画面高
min_rects_area = 1000          # 过滤低于该面积的矩形框
max_rects_area = 140000        # 过滤大于该面积矩形框（全屏163840，留余量）
APPROX_EPSILON_RATIO = 0.06    # approxPolyDP 精度，值越小角点越多
MIN_ASPECT = 0.5               # 仰视梯形变形很大，宽高比放宽到极致
MAX_ASPECT = 3.0              # 仰视梯形变形很大，宽高比放宽到极致

# 角点切换
waypoint_index = 0             # 当前目标角点索引（0~3顺时针）
ALIGN_THRESHOLD = 3            # 对准误差阈值（像素）
STABILITY_FRAMES = 2           # 连续对准帧数
EMA_ALPHA = 0.4                # 激光EMA平滑系数

# ========== Modbus 配置 ==========
MODBUS_BAUD = 115200
MODBUS_SLAVE_ID = 3
# 寄存器地址
REG_TARGET_X    = 0x0000  # 当前目标角点 X
REG_TARGET_Y    = 0x0001  # 当前目标角点 Y
REG_LASER_RED_X = 0x0002  # 红激光 X（0=未检测到）
REG_LASER_RED_Y = 0x0003  # 红激光 Y（0=未检测到）
REG_ERR_X       = 0x0004  # 激光→目标 X 误差
REG_ERR_Y       = 0x0005  # 激光→目标 Y 误差
HOLDING_REGS_COUNT = 10

def find_best_rect(img_cv, laser_pt=None):
    gray = cv2.cvtColor(img_cv, cv2.COLOR_RGB2GRAY)
    blur = cv2.GaussianBlur(gray, (5, 5), 0)

    # 激光掩码：用黑圆盖住激光亮斑，防止它拉高局部自适应阈值炸断白环轮廓
    if laser_pt is not None:
        cv2.circle(blur, laser_pt, 8, (0,), -1)

    # 高斯自适应二值化
    binary = cv2.adaptiveThreshold(
        blur, 255,
        cv2.ADAPTIVE_THRESH_GAUSSIAN_C,
        cv2.THRESH_BINARY,
        31, 12,
    )

    # 形态学闭运算：连接断裂边缘，对内轮廓闭合至关重要
    kernel = cv2.getStructuringElement(cv2.MORPH_RECT, (5, 5))
    binary = cv2.morphologyEx(binary, cv2.MORPH_CLOSE, kernel)

    contours, hierarchy = cv2.findContours(
        binary, cv2.RETR_CCOMP, cv2.CHAIN_APPROX_SIMPLE)

    if hierarchy is None:
        return None
    hierarchy = hierarchy[0]

    outer_best = None
    inner_best = None

    for i, contour in enumerate(contours):
        area = cv2.contourArea(contour)
        if area < min_rects_area or area > max_rects_area:
            continue
        perimeter = cv2.arcLength(contour, True)
        approx = cv2.approxPolyDP(contour, APPROX_EPSILON_RATIO * perimeter, True)
        if len(approx) != 4:
            continue
        x, y, w, h = cv2.boundingRect(approx)
        aspect = w / h
        if aspect <= MIN_ASPECT or aspect > MAX_ASPECT:
            continue
        candidate = {
            "area": area, "rect": (x, y, w, h),
            "center": (x + (w // 2), y + (h // 2)),
            "s": (w * h), "approx": approx
        }
        if hierarchy[i][3] == -1:          # 外轮廓
            if outer_best is None or area > outer_best["area"]:
                outer_best = candidate
        else:                               # 内轮廓（孔洞）
            if inner_best is None or area > inner_best["area"]:
                inner_best = candidate

    if outer_best is None:
        return None

    # 防反转：外轮廓面积必须比内轮廓大（hierarchy 偶尔标反）
    if inner_best is not None and outer_best["area"] < inner_best["area"]:
        outer_best, inner_best = inner_best, outer_best

    return {"outer": outer_best, "inner": inner_best}

# 用于快速验证：读取approx的角点并显示在原图，显示的角点都是红色且没有位置标签
def draw_points(img, points):
    for point in points:
        x, y = int(point[0]), int(point[1])
        img.draw_circle(x, y, 5, image.COLOR_RED, thickness = -1)

# 角点排序，防止approx里的四个角点顺序不对导致处理不了
def order_points(points):
    pts = points.reshape(4, 2).astype("float32")
    ordered = np.zeros((4, 2), dtype = "float32")

    s = pts.sum(axis=1)
    ordered[0] = pts[np.argmin(s)]
    ordered[2] = pts[np.argmax(s)]

    diff= np.diff(pts, axis=1).reshape(4)
    ordered[1]= pts[np.argmin(diff)]
    ordered[3]= pts[np.argmax(diff)]

    return ordered

# 用于进阶处理：画排序后的角点，有颜色且有位置标签
def draw_ordered_points(img, ordered):
    labels = ["TL", "TR", "BR", "BL"]
    colors = [
    image. COLOR_RED,
    image. COLOR_GREEN,
    image. COLOR_BLUE,
    image. COLOR_YELLOW,
    ]
    for i in range(4):
        x, y = int(ordered[i][0]), int(ordered[i][1])
        img.draw_circle(x, y, 5, colors[i], thickness=-1)
        img.draw_string(x + 6, y - 10, labels[i], colors[i])

# 画不横平竖直的贴边四边形框
def draw_polygon(img, ordered):
    pts = ordered.astype("int32")
    for i in range(4):                                             # 0-1-2-3-4-0角点循环画边
        x1, y1 = int(pts[i][0]), int(pts[i][1])
        x2, y2 = int(pts[(i +1)% 4][0]), int(pts[(i +1) % 4][1])   # %4是为了让第四个角点连接到第一个角点
        img.draw_line(x1, y1, x2, y2, image.COLOR_GREEN, thickness = 2)


# ---- 激光检测（findContours只跑一次） ----
def _find_laser(mask, v_ch, s_ch=None):
    """从mask中找最大亮斑（激光），一-pass完成验证+质心计算。None=未找到"""
    contours, _ = cv2.findContours(mask, cv2.RETR_EXTERNAL, cv2.CHAIN_APPROX_SIMPLE)
    if not contours:
        return None
    c = max(contours, key=cv2.contourArea)
    area = cv2.contourArea(c)

    # 亮斑验证：激光→V_内>V_外环；油墨→V_内<V_外环
    cmask = np.zeros(v_ch.shape, dtype=np.uint8)
    cv2.drawContours(cmask, [c], -1, 255, -1)
    v_inside = cv2.mean(v_ch, mask=cmask)[0]
    cmask_d = cv2.dilate(cmask, np.ones((5, 5), np.uint8))
    v_outside = cv2.mean(v_ch, mask=cv2.subtract(cmask_d, cmask))[0]

    if v_inside <= v_outside:
        # 白纸背景回退：高饱和小红点
        if s_ch is not None:
            s_inside = cv2.mean(s_ch, mask=cmask)[0]
            if not (s_inside > 180 and area < 30):
                return None
        else:
            return None

    # 质心
    if area > 0:
        M = cv2.moments(c)
        if M["m00"] > 0:
            return (int(M["m10"] / M["m00"]), int(M["m01"] / M["m00"]))
    if len(c) > 0:
        pt = c[0][0]
        return (int(pt[0]), int(pt[1]))
    return None


def detect_lasers(img_cv, roi=None):
    """红色激光检测"""
    hsv = cv2.cvtColor(img_cv, cv2.COLOR_RGB2HSV)
    ox, oy = 0, 0

    if roi is not None:
        rx, ry, rw, rh = roi
        mx = int(rw * 0.15)
        my = int(rh * 0.15)
        rx = max(0, rx - mx)
        ry = max(0, ry - my)
        rw = min(hsv.shape[1] - rx, rw + 2 * mx)
        rh = min(hsv.shape[0] - ry, rh + 2 * my)
        hsv = hsv[ry:ry+rh, rx:rx+rw]
        ox, oy = rx, ry

    mask_r = cv2.bitwise_or(
        cv2.inRange(hsv, np.array([0, 60, 125]), np.array([10, 255, 255])),
        cv2.inRange(hsv, np.array([170, 60, 125]), np.array([180, 255, 255])))

    pt = _find_laser(mask_r, hsv[:, :, 2], hsv[:, :, 1])
    if pt is not None:
        return (pt[0] + ox, pt[1] + oy)
    return None


# ---- 中线轮廓与路径点生成 ----
def compute_midline(outer_ordered, inner_ordered):
    """计算内外矩形对应角点的中点，返回中线四边形四角点"""
    midline = np.zeros((4, 2), dtype="float32")
    for i in range(4):
        midline[i][0] = (outer_ordered[i][0] + inner_ordered[i][0]) / 2.0
        midline[i][1] = (outer_ordered[i][1] + inner_ordered[i][1]) / 2.0
    return midline


def build_waypoints(corners):
    """直接返回4个角点作为路径点（MCU端做细分插补）"""
    return [(int(c[0]), int(c[1])) for c in corners]


def draw_midline(img, corners):
    """绘制中线四边形（黄色）"""
    for i in range(4):
        x1, y1 = int(corners[i][0]), int(corners[i][1])
        x2, y2 = int(corners[(i + 1) % 4][0]), int(corners[(i + 1) % 4][1])
        img.draw_line(x1, y1, x2, y2, image.COLOR_YELLOW, thickness=2)


def draw_inner_polygon(img, ordered):
    """绘制内矩形（蓝色，区别于外矩形的绿色）"""
    pts = ordered.astype("int32")
    for i in range(4):
        x1, y1 = int(pts[i][0]), int(pts[i][1])
        x2, y2 = int(pts[(i + 1) % 4][0]), int(pts[(i + 1) % 4][1])
        img.draw_line(x1, y1, x2, y2, image.COLOR_BLUE, thickness=2)



# ---- CRC16/MODBUS ----
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


# ---- 裸 UART Modbus 请求处理 ----
def _modbus_handle(u, holding, slave_id):
    """读一次 UART，如果是完整 Modbus 帧就处理并回复"""
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


def run():

    time.sleep(10)
    pinmap.set_pin_function("A16", "UART0_TX")
    pinmap.set_pin_function("A17", "UART0_RX")

    # 裸 UART 替代 libmodbus（后者在 MaixCAM 上 select 不兼容）
    u = uart.UART("/dev/ttyS0", MODBUS_BAUD)
    holding_regs = [0] * HOLDING_REGS_COUNT
    print("Modbus RTU Slave (raw UART) ready")

    global waypoint_index
    cam = camera.Camera(CAM_W, CAM_H)
    disp = display.Display()

    frame_cnt = 0

    # ---- 矩形缓存：摄像头固定、靶子换题时才动，无需每帧检测 ----
    RECT_REFRESH_FRAMES = 400     # 2.5秒刷一次
    RETRY_COOLDOWN = 20            # 没数据时重试间隔（帧）
    cached_waypoints = []
    cached_total_wp = 0
    rect_valid = False
    last_rect_try = -999
    cached_red_pt = None           # 红激光缓存（每3帧刷新，每帧绘制）
    cached_outer_rect = None       # 外框 ROI 缓存，激光检测缩小搜索范围
    consecutive_hit = 0            # 自动切换：连续对准帧计数
    smooth_rx = smooth_ry = 0      # EMA平滑后的激光坐标
    smooth_init = False            # 平滑是否已初始化

    while not app.need_exit():
        img = cam.read()
        img_cv = image.image2cv(img, False, False)
        frame_cnt += 1
        show_img = img

        do_reg_update = False  # 本帧是否有新数据需要写入寄存器

        # ====== 矩形检测 ======
        need_detect = (frame_cnt % RECT_REFRESH_FRAMES == 1) or \
                      (not rect_valid and frame_cnt - last_rect_try >= RETRY_COOLDOWN)
        if need_detect:
            last_rect_try = frame_cnt
            result = find_best_rect(img_cv, cached_red_pt)
            if result is not None:
                best = result["outer"]
                inner = result["inner"]
                outer_ordered = order_points(best["approx"])
                draw_polygon(show_img, outer_ordered)
                draw_ordered_points(show_img, outer_ordered)

                if inner is not None:
                    inner_ordered = order_points(inner["approx"])
                    draw_inner_polygon(show_img, inner_ordered)
                    midline = compute_midline(outer_ordered, inner_ordered)
                    draw_midline(show_img, midline)
                    cached_waypoints = build_waypoints(midline)
                else:
                    cached_waypoints = build_waypoints(outer_ordered)

                cached_total_wp = len(cached_waypoints)
                cached_outer_rect = best["rect"]
                rect_valid = True
                do_reg_update = True
                print(f"[OK] rect refresh, waypoints={cached_total_wp}")
            elif not rect_valid:
                if frame_cnt % 180 == 0:
                    print(f"[FAIL] no rect, frame={frame_cnt}")
            # 已有数据时检测失败 → 不丢旧缓存，但画面闪一下提醒

        # ====== 激光检测（每帧）======
        cached_red_pt = detect_lasers(img_cv, cached_outer_rect)
        if cached_red_pt is None:
            cached_red_pt = detect_lasers(img_cv)
        do_reg_update = True

        # ====== 寄存器写入：矩形或激光任一刷新就写 ======
        if do_reg_update:
            target_x = target_y = 0
            if rect_valid and cached_total_wp > 0:
                wp_idx = waypoint_index % cached_total_wp
                target_x = int(cached_waypoints[wp_idx][0])
                target_y = int(cached_waypoints[wp_idx][1])

            rx = cached_red_pt[0] if cached_red_pt else 0
            ry = cached_red_pt[1] if cached_red_pt else 0
            sx = sy = 0  # 平滑坐标默认值（无激光时）

            # 误差计算 + 对准切换
            if cached_red_pt is not None and rect_valid:
                # EMA平滑激光坐标
                if not smooth_init:
                    smooth_rx, smooth_ry = float(rx), float(ry)
                    smooth_init = True
                else:
                    smooth_rx += EMA_ALPHA * (rx - smooth_rx)
                    smooth_ry += EMA_ALPHA * (ry - smooth_ry)
                sx, sy = int(smooth_rx), int(smooth_ry)

                err_x = target_x - sx
                err_y = target_y - sy
                if frame_cnt % 9 == 0:
                    d = (err_x * err_x + err_y * err_y) ** 0.5
                    print(f"[TRACK] wp#{wp_idx}/{cached_total_wp} | target=({target_x},{target_y}) laser=({sx},{sy}) err=({err_x:+d},{err_y:+d}) d={d:.0f}")

                if abs(err_x) < ALIGN_THRESHOLD and abs(err_y) < ALIGN_THRESHOLD:
                    consecutive_hit += 1
                    if consecutive_hit >= STABILITY_FRAMES:
                        waypoint_index += 1
                        consecutive_hit = 0
                else:
                    consecutive_hit = 0
            else:
                err_x = err_y = 0
                consecutive_hit = 0
                smooth_init = False

            # 写入寄存器
            holding_regs[0:6] = [target_x, target_y, sx, sy, err_x, err_y]

        # 绘制（仅红点+白目标）
        if cached_red_pt is not None:
            show_img.draw_circle(cached_red_pt[0], cached_red_pt[1], 3, image.COLOR_RED, thickness=-1)
        if rect_valid and cached_total_wp > 0:
            wp_idx = waypoint_index % cached_total_wp
            tx = int(cached_waypoints[wp_idx][0])
            ty = int(cached_waypoints[wp_idx][1])
            show_img.draw_circle(tx, ty, 3, image.COLOR_WHITE, thickness=-1)

        if not rect_valid:
            show_img.draw_string(10, 10, "no rect", image.COLOR_RED)

        # ---- 处理 Modbus 请求（每 5 帧查一次 UART）----
        if frame_cnt % 5 == 0:
            _modbus_handle(u, holding_regs, MODBUS_SLAVE_ID)

        fps = time.fps()
        show_img.draw_string(0, 0, f"{fps:.1f}", color=image.COLOR_RED)
        disp.show(show_img)


if __name__ == "__main__":
    run()