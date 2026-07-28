# Modbus RTU Slave 纯裸UART手写版 — 绕过 libmodbus 的 select 兼容问题
from maix import app, time, pinmap, uart, err

# ---- CRC16/MODBUS ----
def crc16(data):
    crc = 0xFFFF
    for b in data:
        crc ^= b
        for _ in range(8):
            if crc & 1:
                crc = (crc >> 1) ^ 0xA001
            else:
                crc >>= 1
    return crc

# ---- 构建 0x03 回复 ----
def build_read_holding_reply(slave_id, start, regs):
    """regs: 从 start 开始的寄存器值列表"""
    n = len(regs)
    resp = bytearray([slave_id, 0x03, n * 2])
    for v in regs:
        resp.append((v >> 8) & 0xFF)
        resp.append(v & 0xFF)
    crc = crc16(resp)
    resp.append(crc & 0xFF)
    resp.append((crc >> 8) & 0xFF)
    return bytes(resp)

# ---- 主程序 ----
time.sleep(3)

pinmap.set_pin_function("A16", "UART0_TX")
pinmap.set_pin_function("A17", "UART0_RX")

u = uart.UART("/dev/ttyS0", 115200)

# 模拟 16 个保持寄存器，填测试值
holding = [0x1111, 0x2222, 0x3333, 0x4444, 0x5555, 0x6666, 0x7777, 0x8888,
           0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000]

print("Modbus RTU Slave (raw UART) ready, addr=1, baud=115200")
print("holding[0..7] = 0x1111~0x8888")
print("Send HEX: 01 03 00 00 00 04 44 09")

while not app.need_exit():
    data = u.read(64)
    if not data or len(data) < 8:
        continue

    sid = data[0]
    func = data[1]

    if sid != 1:
        continue

    if func == 0x03:   # Read Holding Registers
        start = (data[2] << 8) | data[3]
        count = (data[4] << 8) | data[5]
        vals = [holding[start + i] if (start + i) < len(holding) else 0 for i in range(count)]
        reply = build_read_holding_reply(1, start, vals)
        u.write(reply)
        print(f"[OK] READ_HOLDING start={start:#06x} count={count} reply={reply.hex()}")

    elif func == 0x06:  # Write Single Register
        addr = (data[2] << 8) | data[3]
        val = (data[4] << 8) | data[5]
        if addr < len(holding):
            holding[addr] = val
        # 回复 = 原样回显
        crc = crc16(data[:6])
        reply = bytes(data[:6]) + bytes([crc & 0xFF, (crc >> 8) & 0xFF])
        u.write(reply)
        print(f"[OK] WRITE_SINGLE addr={addr:#06x} val={val:#06x}")
