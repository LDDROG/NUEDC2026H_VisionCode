"""
LDD_ROG 2026.7.22  (RS-485 版本)

0x0000 | [0]  | 检测到数字 0 → 置 1, 否则 0
0x0001 | [1]  | 检测到数字 1 → 置 1, 否则 0
...    | ...  | ...
0x0009 | [9]  | 检测到数字 9 → 置 1, 否则 0
0x000A | [10] | ASKING: MCU 写 1 → 重置所有寄存器

MCU 远程重置方法:
  发送 Modbus FC06 (写单个寄存器) 帧:
    从机地址: 0x03
    功能码:   0x06
    寄存器:   0x000A  (高字节 0x00, 低字节 0x0A)
    数据:     0x0001  (高字节 0x00, 低字节 0x01, 写 1 代表重置)
  完整 HEX: 03 06 00 0A 00 01 + CRC16(2字节)
"""

from maix import camera, display, image, nn, app, time, touchscreen
from maix import pinmap, uart, gpio

# ===================== Modbus 通信参数 =====================
MODBUS_BAUD      = 115200
MODBUS_SLAVE_ID  = 3

# ===================== RS-485 方向控制 =====================
RS485_DE_PIN = "A18"

# ===================== 寄存器定义 =====================
REG_DIGIT_START   = 0x0000
REG_ASK           = 0x000A
HOLDING_REGS_COUNT = 11

# ===================== Modbus 帧接收缓冲区 =====================
_rx_buf = bytearray()


def is_in_button(x, y, btn_pos):
    return x > btn_pos[0] and x < btn_pos[0] + btn_pos[2] \
       and y > btn_pos[1] and y < btn_pos[1] + btn_pos[3]


def get_back_btn_img(width):
    ret_width = int(width * 0.1)
    img_back = image.load("/maixapp/share/icon/ret.png")
    w, h = (ret_width, img_back.height() * ret_width // img_back.width())
    if w % 2 != 0:
        w += 1
    if h % 2 != 0:
        h += 1
    img_back = img_back.resize(w, h)
    return img_back


def get_reset_btn_rect(cam_w):
    btn_w = int(cam_w * 0.18)
    btn_h = int(btn_w * 0.55)
    btn_x = cam_w - btn_w - 8
    btn_y = 8
    return [btn_x, btn_y, btn_w, btn_h]


def draw_reset_button(img, btn_rect):
    x, y, w, h = btn_rect
    img.draw_rect(x, y, w, h, color=image.COLOR_WHITE, thickness=2)
    text = "重置"
    font_w = 18
    text_x = x + (w - font_w * 2) // 2
    text_y = y + (h - 20) // 2 + 4
    img.draw_string(text_x, text_y, text, image.COLOR_WHITE)


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


def _rs485_write(u, de_pin, data):
    """
    RS-485 发送: DE拉高 → 写FIFO → 硬件排空 → DE拉低
    """
    de_pin.value(1)                              # 进入发送模式
    u.write(bytes(data))
    # 用 flush 等待硬件 FIFO 全部移出 (比算延时可靠)
    try:
        u.flush()
    except Exception:
        # 如果 Maix UART 没有 flush, 回退到浮点延时 + 余量
        tx_ms = int(len(data) * 10.0 * 1000.0 / MODBUS_BAUD) + 5
        time.sleep_ms(tx_ms)
    de_pin.value(0)                              # 切回接收模式


def _try_parse_frame(u, de_pin, holding, slave_id):
    """
    从接收缓冲区中尝试解析一个完整的 Modbus RTU 帧。
    成功解析并处理 → 返回 True, 从缓冲区移除该帧。
    否则 → 返回 False。
    """
    global _rx_buf

    if len(_rx_buf) < 8:
        return False

    # 跳过前面不匹配的垃圾字节 (寻找从站地址)
    while len(_rx_buf) >= 8 and _rx_buf[0] != slave_id:
        _rx_buf = _rx_buf[1:]

    if len(_rx_buf) < 8 or _rx_buf[0] != slave_id:
        return False

    func = _rx_buf[1]
    if func not in (0x03, 0x06):
        _rx_buf = _rx_buf[1:]                    # 不支持的功能码, 跳过
        return False

    # 取 8 字节帧 (地址1 + 功能码1 + 起始地址2 + 数量/数据2 + CRC2)
    frame = bytes(_rx_buf[:8])

    # 验证 CRC
    crc_expected = _crc16(frame[:6])
    crc_received = frame[6] | (frame[7] << 8)
    if crc_expected != crc_received:
        _rx_buf = _rx_buf[1:]                    # CRC 错, 跳过首字节重试
        return False

    # ---- 响应的帧 (回复前延迟 2ms, 留时间给主站从 TX 切到 RX) ----
    time.sleep_ms(2)

    if func == 0x03:                             # 读保持寄存器
        start = (frame[2] << 8) | frame[3]
        count = (frame[4] << 8) | frame[5]
        resp = bytearray([slave_id, 0x03, count * 2])
        for i in range(count):
            v = holding[start + i] if (start + i) < len(holding) else 0
            resp.append((v >> 8) & 0xFF)
            resp.append(v & 0xFF)
        crc = _crc16(resp)
        resp.append(crc & 0xFF)
        resp.append((crc >> 8) & 0xFF)
        _rs485_write(u, de_pin, resp)

    elif func == 0x06:                           # 写单个寄存器
        addr = (frame[2] << 8) | frame[3]
        val  = (frame[4] << 8) | frame[5]
        if addr < len(holding):
            holding[addr] = val
        crc = _crc16(frame[:6])
        _rs485_write(u, de_pin, frame[:6] + bytes([crc & 0xFF, (crc >> 8) & 0xFF]))

    # 从缓冲区移除已处理的帧
    _rx_buf = _rx_buf[8:]
    return True


def _modbus_handle(u, de_pin, holding, slave_id):
    """
    非阻塞 Modbus 处理:
      1. 读取 UART 收到的所有字节, 追加到缓冲区
      2. 从缓冲区逐帧解析 (支持一帧内积压多帧)
      3. 缓冲区超 256 字节清空 (防异常数据撑爆内存)
    """
    global _rx_buf

    raw = u.read(128)
    if raw:
        _rx_buf.extend(raw)

    if len(_rx_buf) >= 8:
        while _try_parse_frame(u, de_pin, holding, slave_id):
            pass

    if len(_rx_buf) > 256:
        _rx_buf.clear()


def main(disp):
    time.sleep(10)

    # ---- 初始化 UART + RS-485 ----
    pinmap.set_pin_function("A16", "UART0_TX")
    pinmap.set_pin_function("A17", "UART0_RX")
    u = uart.UART("/dev/ttyS0", MODBUS_BAUD)

    de_pin = gpio.GPIO(RS485_DE_PIN, gpio.Mode.OUT)
    de_pin.value(0)

    # ---- 初始化保持寄存器 ----
    holding_regs = [0] * HOLDING_REGS_COUNT
    print("Modbus RTU Slave (RS-485) ready, slave_id=%d" % MODBUS_SLAVE_ID)

    # ---- 加载 OCR 模型 ----
    model = "/root/models/pp_ocr.mud"
    ocr = nn.PP_OCR(model)

    # ---- 初始化摄像头 ----
    cam = camera.Camera(ocr.input_width(), ocr.input_height(), ocr.input_format())

    # ---- 初始化触摸屏 ----
    ts = touchscreen.TouchScreen()

    # ---- 返回按钮 ----
    img_back = get_back_btn_img(cam.width())
    back_rect = [0, 0, img_back.width(), img_back.height()]
    back_rect_disp = image.resize_map_pos(
        cam.width(), cam.height(),
        disp.width(), disp.height(),
        image.Fit.FIT_CONTAIN,
        back_rect[0], back_rect[1], back_rect[2], back_rect[3]
    )

    # ---- 重置按钮 ----
    reset_rect = get_reset_btn_rect(cam.width())
    reset_rect_disp = image.resize_map_pos(
        cam.width(), cam.height(),
        disp.width(), disp.height(),
        image.Fit.FIT_CONTAIN,
        reset_rect[0], reset_rect[1], reset_rect[2], reset_rect[3]
    )

    # ---- 字体 ----
    image.load_font("ppocr", "/maixapp/share/font/ppocr_keys_v1.ttf", size=20)
    image.set_default_font("ppocr")

    # ---- 主循环 ----
    print("Main loop started.")
    reset_btn_prev = False
    back_btn_prev  = False
    while not app.need_exit():
        img = cam.read()

        # -------- OCR 检测 --------
        objs = ocr.detect(img)
        recognized = None
        for obj in objs:
            char_str = obj.char_str().strip()
            if not (char_str.isdigit() and len(char_str) == 1):
                continue
            digit = int(char_str)
            if recognized is None:
                recognized = digit
            points = obj.box.to_list()
            img.draw_keypoints(points, image.COLOR_RED, 4, -1, 1)
            img.draw_string(obj.box.x4, obj.box.y4, char_str, image.COLOR_RED)

        if recognized is not None:
            for i in range(10):
                holding_regs[REG_DIGIT_START + i] = 0
            holding_regs[REG_DIGIT_START + recognized] = 1
            print("[OCR] digit=%d, reg[%d]=1, all others=0" % (recognized, recognized))

        # -------- 重置处理 --------
        need_reset = False
        x, y, pressed = ts.read()

        reset_btn_now = pressed and is_in_button(x, y, reset_rect_disp)
        if reset_btn_now and not reset_btn_prev:
            need_reset = True
            print("[Touch] Reset button pressed")
        reset_btn_prev = reset_btn_now

        if holding_regs[REG_ASK] != 0:
            need_reset = True
            holding_regs[REG_ASK] = 0
            print("[Modbus] ASKING register triggered reset")

        if need_reset:
            for i in range(10):
                holding_regs[REG_DIGIT_START + i] = 0
            print("[Reset] All digit registers cleared to 0")
            img.draw_string(10, 10, "重置完成", image.COLOR_GREEN)

        # -------- UI --------
        draw_reset_button(img, reset_rect)
        img.draw_image(0, 0, img_back)
        disp.show(img)

        # -------- Modbus 处理 --------
        _modbus_handle(u, de_pin, holding_regs, MODBUS_SLAVE_ID)

        # -------- 退出 --------
        back_btn_now = pressed and is_in_button(x, y, back_rect_disp)
        if back_btn_now and not back_btn_prev:
            print("Exit button pressed. Exiting...")
            app.set_exit_flag(True)
        back_btn_prev = back_btn_now


def run():
    screen = display.Display()
    try:
        main(screen)
    except Exception:
        import traceback
        e = traceback.format_exc()
        print(e)
        img = image.Image(screen.width(), screen.height())
        img.draw_string(2, 2, e, image.COLOR_WHITE, font="hershey_complex_small", scale=0.6)
        screen.show(img)
        while not app.need_exit():
            time.sleep(0.2)
