# LDD_ROG 2026.7.29
# -*- coding: utf-8 -*-
# MaixCAM必须先上电开机再给MCU上电，不然串口反流会开不了机
# 接二极管没用的别试了，MaixCAM的串口很容易受二极管压降影响，接了就收不到数据了，稳妥一点优先上电
# 换模型只需改: MODEL_PATH, CAM_W/CAM_H, nn.YOLOxx(), FOV_WIDTH_CM

from maix import app, camera, display, image, nn, time, touchscreen
from maix import pinmap, uart
import math

CAM_W = 320                      # 摄像头分辨率设为与模型严格一致
CAM_H = 64                       # 经过测试，本项目的模型在MaixCAM的最佳分辨率是320x64，此时钢球稳定检测的最远距离约为30cm，帧率约为50FPS
MODEL_PATH = "/模型.mud"
CONF_TH = 0.36                   # 置信度判定阈值

# 水管标定参数
FOV_WIDTH_CM = 28                # 相机视野在水管平面覆盖的最大水平宽度(cm)
PIPE_LEFT_CM = 2.3               # 水管左端距画面左边缘(cm)
PIPE_RIGHT_CM = 0.5              # 水管右端距画面右边缘(cm)

PX_PER_CM = CAM_W / FOV_WIDTH_CM
PIPE_CENTER_X = int(PX_PER_CM * (PIPE_LEFT_CM + (FOV_WIDTH_CM - PIPE_LEFT_CM - PIPE_RIGHT_CM) / 2))
PIPE_CENTER_Y = CAM_H // 2       # 这里默认水管的垂直位置在画面中央。如果有偏移需要另外算。
PIPE_ANGLE_DEG = 0               # 水管中轴线与图像X轴的夹角(°)，逆时针为正。

# 串口
UART_BAUD = 115200

def pack_frame(error_px, vel_cm_s, status):
    frame = bytearray(8)
    frame[0] = 0xAA
    frame[1] = error_px & 0xFF
    frame[2] = (error_px >> 8) & 0xFF
    frame[3] = vel_cm_s & 0xFF
    frame[4] = (vel_cm_s >> 8) & 0xFF
    frame[5] = status & 0xFF
    cs = 0
    for i in range(6):
        cs ^= frame[i]
    frame[6] = cs
    frame[7] = 0x55
    return bytes(frame)

COLOR_GREEN  = image.Color.from_rgb(0, 255, 0)
COLOR_RED    = image.Color.from_rgb(255, 0, 0)
COLOR_YELLOW = image.Color.from_rgb(255, 255, 0)
COLOR_BLUE   = image.Color.from_rgb(0, 128, 255)
COLOR_CYAN   = image.Color.from_rgb(0, 255, 255)

# 计算钢球误差
def _calc_raw_error(ball_cx, ball_cy, center_x, center_y, angle_deg):
    dx = ball_cx - center_x
    dy = ball_cy - center_y
    dist = int(math.sqrt(dx * dx + dy * dy))
    if dist == 0:
        return 0
    rad = math.radians(angle_deg)
    proj = dx * math.cos(rad) + dy * math.sin(rad)
    if proj >= 0:
        return dist 
    else: 
        return -dist

# 滤波
ALPHA = 0.7                       # 位置平滑系数
BETA  = 0.3                       # 速度跟踪系数
MAX_VEL = 1500                    # 速度限幅 (px/s)，防止电机冲太猛。电机那边也需要加限幅才保险一点。

class AlphaBetaTracker:
    def __init__(self, alpha, beta, max_vel):
        self.alpha = alpha
        self.beta = beta
        self.max_vel = max_vel
        self.x = 0.0              # 滤波后位置 (px, 有符号)
        self.v = 0.0              # 滤波后速度 (px/s, 有符号)
        self.last_ms = 0
        self.ready = False

    def update(self, z, ts_ms):
        if not self.ready:
            self.x = float(z)
            self.v = 0.0
            self.last_ms = ts_ms
            self.ready = True
            return self.x, self.v

        dt = (ts_ms - self.last_ms) / 1000.0
        if dt <= 0.001:
            return self.x, self.v

        x_pred = self.x + self.v * dt              # 预测位置
        residual = z - x_pred                      # 测量残差
        self.x = x_pred + self.alpha * residual    # 修正位置

        # 残差死区: |residual|<2px视为噪声，不刷新速度
        if abs(residual) >= 2:
            self.v = self.v + self.beta * residual / dt
            if self.v > self.max_vel:   self.v = self.max_vel
            if self.v < -self.max_vel:  self.v = -self.max_vel
        self.last_ms = ts_ms
        return self.x, self.v

    def coast(self, ts_ms):
        if not self.ready:       # 未检测到钢球时速度自然衰减
            return 0.0, 0.0
        dt = (ts_ms - self.last_ms) / 1000.0
        if 0 < dt < 0.5:
            self.x = self.x + self.v * dt
            self.v = self.v * 0.95
        self.last_ms = ts_ms
        return self.x, self.v

# MaixCAM检测结果画图
def draw_detection(img, obj, center_x, center_y, error_px):
    x, y, bw, bh = obj.x, obj.y, obj.w, obj.h
    cx = x + bw // 2
    cy = y + bh // 2
    img.draw_rect(x, y, bw, bh, color=COLOR_GREEN, thickness=2)

def run():
    print("哒哒哒哒哒" * 5)
    print(f"模型: {MODEL_PATH}")
    print(f"分辨率: {CAM_W}x{CAM_H}")
    print(f"串口: /dev/ttyS0 @ {UART_BAUD}")
    print("好想玩原神" * 5)

    # 串口初始化，默认UART0，注意需要等待打印完开机日志才能开始通讯
    time.sleep(8)
    pinmap.set_pin_function("A16", "UART0_TX")
    pinmap.set_pin_function("A17", "UART0_RX")      # 开环用不到这个
    u = uart.UART("/dev/ttyS0", UART_BAUD)
    print("UART TX ready")

    # 相机初始化
    cam = camera.Camera(CAM_W, CAM_H)
    disp = display.Display()
    ts = touchscreen.TouchScreen()

    print("加载YOLO模型...")
    detector = nn.YOLO26(model=MODEL_PATH, dual_buff=True)
    print(f"模型加载完成! 输入: {detector.input_width()}x{detector.input_height()}")



    frame_count = 0
    last_status_print = 0
    center_x = PIPE_CENTER_X         
    center_y = PIPE_CENTER_Y
    arbitrary_mode = False       # 任意位置模式
    tracker = AlphaBetaTracker(ALPHA, BETA, MAX_VEL)
    last_ms = time.time_ms()

    while not app.need_exit():
        img = cam.read()
        if img is None:
            continue

        objs = detector.detect(img, conf_th=CONF_TH)
        now = time.time_ms()



        confidence = 0
        status = 0
        error_px = 0
        vel_cm_s = 0

        if len(objs) >= 1:
            obj = objs[0] if len(objs) == 1 else max(objs, key=lambda o: o.score)
            ball_cx = obj.x + obj.w // 2
            ball_cy = obj.y + obj.h // 2
            confidence = int(obj.score * 1000)
            status = min(len(objs), 2)

            raw_z = _calc_raw_error(ball_cx, ball_cy, center_x, center_y, PIPE_ANGLE_DEG)
            filt_x, filt_v = tracker.update(raw_z, now)
            error_px = int(filt_x) if filt_x >= 0 else -int(abs(filt_x))
            vel_cm_s = int(filt_v / PX_PER_CM * 100)

            draw_detection(img, obj, center_x, center_y, error_px)
        else:
            filt_x, filt_v = tracker.coast(now)
            error_px = int(filt_x) if filt_x >= 0 else -int(abs(filt_x))
            vel_cm_s = int(filt_v / PX_PER_CM * 100)

        # 串口数据推送
        u.write(pack_frame(error_px, vel_cm_s, status))

        # 调试输出
        frame_count += 1
        if status != 0 and frame_count - last_status_print >= 30:
            print(f"[{frame_count:04d}] err={error_px:+d}px "
                  f"vel={vel_cm_s/100:.2f}cm/s conf={confidence/1000:.2f}")
            last_status_print = frame_count

        fps = 1000.0 / max(now - last_ms, 1)
        last_ms = now
        if frame_count % 30 == 0:
            img.draw_string(0, 0, f"FPS:{fps:.1f}", color=COLOR_YELLOW, scale=1.2)



        dot_color = COLOR_BLUE if arbitrary_mode else COLOR_RED
        img.draw_circle(center_x, center_y, 2, color=dot_color, thickness=-1)



        t = ts.read()
        tx_c, ty_c = 0, 0
        if t[2]:
            tx_c, ty_c = image.resize_map_pos_reverse(
                img.width(), img.height(),
                disp.width(), disp.height(),
                image.Fit.FIT_CONTAIN,
                t[0], t[1])

        bx_r, by_r = img.width() - 14, 2
        img.draw_rect(bx_r, by_r, 12, 12, color=COLOR_RED, thickness=-1)
        if t[2] and bx_r <= tx_c <= bx_r + 12 and by_r <= ty_c <= by_r + 12:
            print("触控退出")
            break

        bx_b, by_b = 2, 2
        img.draw_rect(bx_b, by_b, 12, 12, color=COLOR_BLUE, thickness=-1)
        if t[2] and bx_b <= tx_c <= bx_b + 12 and by_b <= ty_c <= by_b + 12:
            arbitrary_mode = not arbitrary_mode
            tracker = AlphaBetaTracker(ALPHA, BETA, MAX_VEL)
            print(f"任意位置模式: {'ON' if arbitrary_mode else 'OFF'}  目标 x={center_x}")

        if arbitrary_mode and t[2]:
            on_exit = bx_r <= tx_c <= bx_r + 12 and by_r <= ty_c <= by_r + 12
            on_mode = bx_b <= tx_c <= bx_b + 12 and by_b <= ty_c <= by_b + 12
            if not on_exit and not on_mode:
                center_x = tx_c
                tracker = AlphaBetaTracker(ALPHA, BETA, MAX_VEL)
                print(f"目标位置更新: x={center_x}")

        disp.show(img)