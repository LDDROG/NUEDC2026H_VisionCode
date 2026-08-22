# 简单串口回环测试：收到什么就发回什么，同时每秒发一条心跳
from maix import app, time, pinmap, uart, err

time.sleep(3)

# 设置 A16=TX, A17=RX
r1 = pinmap.set_pin_function("A16", "UART0_TX")
r2 = pinmap.set_pin_function("A17", "UART0_RX")
print(f"pinmap TX={r1}, RX={r2}")

# 初始化 UART0: 115200 8N1
u = uart.UART("/dev/ttyS0", 115200)
print("UART0 init done, waiting for data...")

last_beat = time.ticks_ms()

while not app.need_exit():
    # 收：有数据就读，读到了原样发回
    data = u.read(64)           # 最多读 64 字节
    if data and len(data) > 0:
        u.write(data)           # 回环：收到什么发什么
        print(f"[RX:{len(data)}] {data.hex()}")

    # 每秒发一次心跳
    '''
    now = time.ticks_ms()
    if now - last_beat > 1000:
        u.write(b"HELLO\n")
        print("[TX] HELLO")
        last_beat = now
        '''
