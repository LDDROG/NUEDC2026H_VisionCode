from maix import camera, display, image, nn, app, time, touchscreen
from maix import pinmap, uart

MODBUS_BAUD = 115200
MODBUS_SLAVE_ID = 3
REG_DIGIT_BASE = 0x0000
HOLDING_REGS_COUNT = 10

def is_in_button(x, y, btn_pos):
    return x > btn_pos[0] and x < btn_pos[0] + btn_pos[2] and y > btn_pos[1] and y < btn_pos[1] + btn_pos[3]

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


def _modbus_handle(u, holding, slave_id):
    raw = u.read(64)
    if not raw or len(raw) < 8:
        return
    if raw[0] != slave_id:
        return

    func = raw[1]
    if func == 0x03:
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

    elif func == 0x06:
        addr = (raw[2] << 8) | raw[3]
        val = (raw[4] << 8) | raw[5]
        if addr < len(holding):
            holding[addr] = val
        crc = _crc16(raw[:6])
        u.write(bytes(raw[:6]) + bytes([crc & 0xFF, (crc >> 8) & 0xFF]))


def main(disp):
    time.sleep(10)
    pinmap.set_pin_function("A16", "UART0_TX")
    pinmap.set_pin_function("A17", "UART0_RX")
    u = uart.UART("/dev/ttyS0", MODBUS_BAUD)
    holding_regs = [0] * HOLDING_REGS_COUNT
    print("Modbus RTU Slave (raw UART) ready")

    model = "/root/models/pp_ocr.mud"
    ocr = nn.PP_OCR(model)

    cam = camera.Camera(ocr.input_width(), ocr.input_height(), ocr.input_format())
    ts = touchscreen.TouchScreen()
    img_back = get_back_btn_img(cam.width())
    back_rect = [0, 0, img_back.width(), img_back.height()]
    back_rect_disp = image.resize_map_pos(cam.width(), cam.height(), disp.width(), disp.height(), image.Fit.FIT_CONTAIN, back_rect[0], back_rect[1], back_rect[2], back_rect[3])

    image.load_font("ppocr", "/maixapp/share/font/ppocr_keys_v1.ttf", size = 20)
    image.set_default_font("ppocr")

    while not app.need_exit():
        img = cam.read()
        objs = ocr.detect(img)
        for i in range(10):     # 10个寄存器对应10个数字
            holding_regs[i] = 0
        for obj in objs:
            points = obj.box.to_list()
            img.draw_keypoints(points, image.COLOR_RED, 4, -1, 1)
            char_str = obj.char_str().strip()
            img.draw_string(obj.box.x4, obj.box.y4, char_str, image.COLOR_RED)
            if char_str.isdigit() and len(char_str) == 1:
                digit = int(char_str)
                holding_regs[REG_DIGIT_BASE + digit] = 1
                print(f"[OCR] digit={digit}")
        img.draw_image(0, 0, img_back)
        disp.show(img)
        _modbus_handle(u, holding_regs, MODBUS_SLAVE_ID)
        x, y, pressed = ts.read()
        if is_in_button(x, y, back_rect_disp):
            app.set_exit_flag(True)




if __name__ == '__main__':
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