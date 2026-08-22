"""
LDD_ROG 2026.7.22

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
from maix import pinmap, uart

# ===================== Modbus 通信参数 =====================
MODBUS_BAUD      = 115200        # 串口波特率 (与 MCU 一致)
MODBUS_SLAVE_ID  = 3             # 本机 Modbus 从站地址

# ===================== 寄存器定义 =====================
REG_DIGIT_START   = 0x0000        # 数字寄存器的起始地址 (0x0000 ~ 0x0009, 共10个)
REG_ASK          = 0x000A        # ASKING 寄存器地址: MCU 写 1 触发重置
HOLDING_REGS_COUNT = 11          # 保持寄存器总数: 10(数字) + 1(ASKING)


def is_in_button(x, y, btn_pos):
    """
    判断触摸坐标 (x, y) 是否落在按钮区域内
    btn_pos 格式: [左上角x, 左上角y, 宽度, 高度]
    """
    return x > btn_pos[0] and x < btn_pos[0] + btn_pos[2] \
       and y > btn_pos[1] and y < btn_pos[1] + btn_pos[3]


def get_back_btn_img(width):
    """
    加载返回图标并缩放到适合摄像头画面的大小
    width: 摄像头画面宽度, 图标宽度取画面宽度的 10%
    返回缩放后的图标 image 对象
    """
    ret_width = int(width * 0.1)                            # 图标宽度 = 画面宽 10%
    img_back = image.load("/maixapp/share/icon/ret.png")    # 加载返回图标
    w, h = (ret_width, img_back.height() * ret_width // img_back.width())  # 等比缩放高度
    if w % 2 != 0:          # 宽度对齐到偶数 (硬件要求)
        w += 1
    if h % 2 != 0:          # 高度对齐到偶数
        h += 1
    img_back = img_back.resize(w, h)   # 缩放
    return img_back


def get_reset_btn_rect(cam_w):
    """
    生成"重置"按钮在摄像头坐标系中的位置和大小
    cam_w: 摄像头画面宽度
    返回: [x, y, w, h] → 右上角, 宽约 20%, 高约 10%
    """
    btn_w = int(cam_w * 0.18)           # 按钮宽度 = 画面宽 18%
    btn_h = int(btn_w * 0.55)           # 按钮高度 = 宽 * 0.55 (约 2 个汉字高度)
    btn_x = cam_w - btn_w - 8           # 右对齐, 留 8px 边距
    btn_y = 8                           # 贴顶部, 留 8px 边距
    return [btn_x, btn_y, btn_w, btn_h]


def draw_reset_button(img, btn_rect):
    """
    在画面上绘制"重置"按钮
    img:      摄像头画面
    btn_rect: [x, y, w, h] 按钮位置和大小
    """
    x, y, w, h = btn_rect
    # 按钮边框 (白色)
    img.draw_rect(x, y, w, h, color=image.COLOR_WHITE, thickness=2)
    # 按钮文字 "重置", 居中放置
    text = "重置"
    # 文字宽度约等于 2个中文字 × 字体大小, 大致居中
    font_w = 18                         # 预估每个汉字宽约 18px (对应 size=20 的字体)
    text_x = x + (w - font_w * 2) // 2  # 水平居中
    text_y = y + (h - 20) // 2 + 4      # 垂直居中 (额外 +4 微调基线)
    img.draw_string(text_x, text_y, text, image.COLOR_WHITE)


def _crc16(data):
    """
    Modbus RTU CRC16 校验算法
    多项式: 0xA001 (Modbus 标准)
    data: 需要计算校验的字节数据
    返回 16 位 CRC 值
    """
    crc = 0xFFFF                        # CRC 初始值
    for b in data:                      # 遍历每个字节
        crc ^= b                        # 与当前字节异或
        for _ in range(8):              # 处理 8 位
            if crc & 1:                 # 最低位为 1
                crc = (crc >> 1) ^ 0xA001   # 右移 + 异或多项式
            else:
                crc >>= 1               # 否则只右移
    return crc


def _modbus_handle(u, holding, slave_id):
    """
    处理 Modbus RTU 请求 (非阻塞, 无请求时直接返回)
    支持功能码:
      FC03 (0x03) — 读保持寄存器
      FC06 (0x06) — 写单个寄存器

    u:        UART 对象
    holding:  保持寄存器列表 (长度为 HOLDING_REGS_COUNT)
    slave_id: 本机从站地址
    """
    raw = u.read(64)                    # 尝试读取最多 64 字节
    if not raw or len(raw) < 8:         # 请求帧至少 8 字节 (地址1+功能码1+起始2+数量2+CRC2)
        return                          # 没有完整帧, 直接返回
    if raw[0] != slave_id:              # 从站地址不匹配, 不是发给我的
        return

    func = raw[1]                       # 功能码: 0x03 或 0x06

    # ---- FC03: 读保持寄存器 ----
    if func == 0x03:
        start = (raw[2] << 8) | raw[3]  # 起始寄存器地址 (大端序)
        count = (raw[4] << 8) | raw[5]  # 要读取的寄存器数量
        # 构造响应帧: 地址 + 功能码 + 字节数 + 数据 + CRC
        resp = bytearray([slave_id, 0x03, count * 2])
        for i in range(count):
            # 读取寄存器值, 越界则返回 0
            v = holding[start + i] if (start + i) < len(holding) else 0
            resp.append((v >> 8) & 0xFF)  # 高字节
            resp.append(v & 0xFF)        # 低字节
        crc = _crc16(resp)
        resp.append(crc & 0xFF)          # CRC 低字节
        resp.append((crc >> 8) & 0xFF)   # CRC 高字节
        u.write(bytes(resp))

    # ---- FC06: 写单个寄存器 ----
    elif func == 0x06:
        addr = (raw[2] << 8) | raw[3]   # 目标寄存器地址
        val  = (raw[4] << 8) | raw[5]   # 要写入的值
        if addr < len(holding):         # 地址有效则写入
            holding[addr] = val
        # 响应: 原样回显请求的前 6 字节 + CRC
        crc = _crc16(raw[:6])
        u.write(bytes(raw[:6]) + bytes([crc & 0xFF, (crc >> 8) & 0xFF]))


def main(disp):
    """
    主函数:
      1. 初始化硬件 (UART / 摄像头 / 触摸屏 / OCR 模型)
      2. 循环: OCR 识别 → 更新寄存器 → 处理重置 → 显示画面
    """
    # ---- 上电延时, 等待外设就绪 ----
    time.sleep(10)

    # ---- 初始化 UART (Modbus RTU 从站) ----
    pinmap.set_pin_function("A16", "UART0_TX")  # 引脚映射: A16 → UART0 发送
    pinmap.set_pin_function("A17", "UART0_RX")  # 引脚映射: A17 → UART0 接收
    u = uart.UART("/dev/ttyS0", MODBUS_BAUD)    # 打开串口设备

    # ---- 初始化保持寄存器 (全部从 0 开始) ----
    holding_regs = [0] * HOLDING_REGS_COUNT
    print("Modbus RTU Slave ready, slave_id=%d" % MODBUS_SLAVE_ID)

    # ---- 加载 OCR 模型 (PP_OCR) ----
    model = "/root/models/pp_ocr.mud"
    ocr = nn.PP_OCR(model)

    # ---- 初始化摄像头 (分辨率与 OCR 模型匹配) ----
    cam = camera.Camera(ocr.input_width(), ocr.input_height(), ocr.input_format())

    # ---- 初始化触摸屏 ----
    ts = touchscreen.TouchScreen()

    # ---- 加载返回按钮图标 (左上角) ----
    img_back = get_back_btn_img(cam.width())
    back_rect = [0, 0, img_back.width(), img_back.height()]  # 摄像头坐标系中的位置

    # 将摄像头坐标映射到显示屏坐标, 用于触摸检测
    # 因为摄像头画面以 FIT_CONTAIN 方式缩放显示, 所以触摸点需要做坐标转换
    back_rect_disp = image.resize_map_pos(
        cam.width(), cam.height(),            # 源: 摄像头尺寸
        disp.width(), disp.height(),          # 目标: 显示屏尺寸
        image.Fit.FIT_CONTAIN,                # 缩放方式
        back_rect[0], back_rect[1], back_rect[2], back_rect[3]
    )

    # ---- 重置按钮 (右上角) ----
    reset_rect = get_reset_btn_rect(cam.width())
    # 同样映射到显示坐标
    reset_rect_disp = image.resize_map_pos(
        cam.width(), cam.height(),
        disp.width(), disp.height(),
        image.Fit.FIT_CONTAIN,
        reset_rect[0], reset_rect[1], reset_rect[2], reset_rect[3]
    )

    # ---- 加载汉字字体 (用于在摄像头画面上叠加文字) ----
    image.load_font("ppocr", "/maixapp/share/font/ppocr_keys_v1.ttf", size=20)
    image.set_default_font("ppocr")

    # ---- 主循环 ----
    print("Main loop started. Ready to recognize digits 0-9.")
    reset_btn_prev = False      # 上一帧是否按住重置按钮 (下降沿检测用)
    back_btn_prev  = False      # 同上, 返回按钮用
    while not app.need_exit():
        # -------- 1. 读取摄像头帧 --------
        img = cam.read()

        # -------- 2. OCR 检测 & 更新寄存器 (只识别单个数字 0-9) --------
        objs = ocr.detect(img)          # OCR 识别, 返回检测到的所有文字对象

        recognized = None               # 本轮识别到的第一个有效数字
        for obj in objs:
            char_str = obj.char_str().strip()   # 获取识别文本, 去除首尾空白

            # 过滤: 只接受单个数字 (0-9)
            if not (char_str.isdigit() and len(char_str) == 1):
                continue                        # 跳过中文 / 英文 / 多字符

            digit = int(char_str)               # 转为整数
            if recognized is None:
                recognized = digit              # 取第一个有效数字用于寄存器更新

            # 画框和文字标注 (只画数字, 中文英文不画)
            points = obj.box.to_list()
            img.draw_keypoints(points, image.COLOR_RED, 4, -1, 1)  # 红色边框
            img.draw_string(obj.box.x4, obj.box.y4, char_str, image.COLOR_RED)  # 识别文字

        # 识别到数字 → 对应寄存器置 1，其余全部置 0
        if recognized is not None:
            for i in range(10):
                holding_regs[REG_DIGIT_START + i] = 0
            holding_regs[REG_DIGIT_START + recognized] = 1
            print("[OCR] digit=%d, reg[%d]=1, all others=0" % (recognized, recognized))

        # -------- 3. 处理重置 --------
        need_reset = False

        x, y, pressed = ts.read()                           # 读取触摸屏状态

        # 3a. 触摸"重置"按钮? (按下瞬间触发)
        success_text = "重置完成"
        reset_btn_now = pressed and is_in_button(x, y, reset_rect_disp)

        if reset_btn_now and not reset_btn_prev:               # 按下瞬间 (上升沿)
            need_reset = True
            print("[Touch] Reset button pressed")
        reset_btn_prev = reset_btn_now                         # 记住状态给下一帧

        # 3b. MCU 通过 Modbus 写 ASKING 寄存器触发了重置?
        if holding_regs[REG_ASK] != 0:                      # MCU 写入了非零值
            need_reset = True
            holding_regs[REG_ASK] = 0                       # 清除 ASKING, 等待下次触发
            print("[Modbus] ASKING register triggered reset")

        # 3c. 执行重置: 清空所有数字寄存器 (0-9)
        if need_reset:
            for i in range(10):                             # 索引 0 ~ 9 全部归零
                holding_regs[REG_DIGIT_START + i] = 0
            print("[Reset] All digit registers cleared to 0")
            img.draw_string(10, 10, success_text, image.COLOR_GREEN)

        # -------- 4. 绘制 UI 元素 --------
        # 重置按钮
        draw_reset_button(img, reset_rect)
        # 返回按钮 (左上角)
        img.draw_image(0, 0, img_back)

        # -------- 5. 显示画面 --------
        disp.show(img)

        # -------- 6. 处理 Modbus 请求 --------
        _modbus_handle(u, holding_regs, MODBUS_SLAVE_ID)

        # -------- 7. 检查返回按钮 (松手退出, 防误触) --------
        back_btn_now = pressed and is_in_button(x, y, back_rect_disp)
        if back_btn_now and not back_btn_prev:
            print("Exit button pressed. Exiting...")
            app.set_exit_flag(True)
        back_btn_prev = back_btn_now


# ===================== 程序入口 =====================
def run():
    screen = display.Display()
    try:
        main(screen)
    except Exception:
        import traceback
        e = traceback.format_exc()
        print(e)
        # 异常时在屏幕上显示错误信息
        img = image.Image(screen.width(), screen.height())
        img.draw_string(2, 2, e, image.COLOR_WHITE, font="hershey_complex_small", scale=0.6)
        screen.show(img)
        while not app.need_exit():
            time.sleep(0.2)
