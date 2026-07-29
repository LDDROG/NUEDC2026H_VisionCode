"""
YOLO11s 钢球检测 — MaixCam 端实时推理 (HighFPS)
模型: yolo11s_ironball_highfps.mud (320x224, INT8)
"""

from maix import camera, display, image, nn, app, time

MODEL_PATH = "/root/models/yolo11s_ironball_highfps.mud"
CONF_TH = 0.4

# 预分配颜色，避免每帧创建对象
COLOR_GREEN  = image.Color.from_rgb(0, 255, 0)
COLOR_RED    = image.Color.from_rgb(255, 0, 0)
COLOR_YELLOW = image.Color.from_rgb(255, 255, 0)


def draw_results(img, objs):
    for obj in objs:
        x, y, bw, bh = obj.x, obj.y, obj.w, obj.h
        img.draw_rect(x, y, bw, bh, color=COLOR_GREEN, thickness=2)
        img.draw_circle(x + bw // 2, y + bh // 2, 3, color=COLOR_RED, thickness=-1)


def main():
    print("=" * 40)
    print("YOLO11s 钢球检测 (HighFPS)")
    print(f"模型: {MODEL_PATH}")
    print("=" * 40)

    cam = camera.Camera(320, 224)   # 矩形画幅,匹配25cm水管+模型输入
    disp = display.Display()

    print("加载模型中...")
    detector = nn.YOLO11(model=MODEL_PATH, dual_buff=True)  
    print(f"模型加载完成! 输入尺寸: {detector.input_width()}x{detector.input_height()}")

    last_ms = time.time_ms()
    frame_count = 0
    last_count = -1

    while not app.need_exit():
        img = cam.read()
        if img is None:
            continue

        objs = detector.detect(img, conf_th=CONF_TH)

        cur = len(objs)
        if cur != last_count:
            print(f"[{frame_count:04d}] 检测到 {cur} 个钢球")
            for i, obj in enumerate(objs):
                cx = obj.x + obj.w // 2
                cy = obj.y + obj.h // 2
                print(f"  [{i}] ({cx},{cy}) {obj.w}x{obj.h} conf={obj.score:.3f}")
            last_count = cur

        draw_results(img, objs)

        frame_count += 1
        now = time.time_ms()
        fps = 1000.0 / max(now - last_ms, 1)
        last_ms = now
        img.draw_string(10, 2, f"FPS:{fps:.1f}",
                       color=COLOR_YELLOW, scale=1.5)

        disp.show(img)


if __name__ == "__main__":
    try:
        main()
    except Exception as e:
        print(f"错误: {e}")
        import traceback
        traceback.print_exc()
