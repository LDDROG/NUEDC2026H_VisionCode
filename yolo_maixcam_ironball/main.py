"""
YOLO26s 钢球检测 — MaixCam 端实时推理
模型: yolo26s_ironball_320x224.mud (320x224, INT8)
"""

from maix import camera, display, image, nn, app, time

MODEL_PATH = "/root/models/yolo26s_ironball_320x224.mud"
CONF_TH = 0.5
IOU_TH = 0.45
CENTER_LINE = True      # 画中心十字


def draw_results(img, objs):
    h, w = img.height(), img.width()
    if CENTER_LINE:
        cx, cy = w // 2, h // 2
        img.draw_line(cx, 0, cx, h, color=image.Color.from_rgb(100, 100, 100), thickness=1)
        img.draw_line(0, cy, w, cy, color=image.Color.from_rgb(100, 100, 100), thickness=1)

    for obj in objs:
        x, y, bw, bh = obj.x, obj.y, obj.w, obj.h
        conf = obj.score
        cx, cy = x + bw // 2, y + bh // 2

        img.draw_rect(x, y, bw, bh, color=image.Color.from_rgb(0, 255, 0), thickness=2)
        img.draw_circle(cx, cy, 3, color=image.Color.from_rgb(255, 0, 0), thickness=-1)
        img.draw_string(x, y - 16, f"iron {conf:.2f}",
                       color=image.Color.from_rgb(0, 255, 0), scale=1.2)
    return img


def main():
    print("=" * 40)
    print("YOLO26s 钢球检测")
    print(f"模型: {MODEL_PATH}")
    print("=" * 40)

    cam = camera.Camera(640, 480)
    disp = display.Display()

    print("加载模型中...")
    detector = nn.YOLO26(model=MODEL_PATH, dual_buff=True)
    print(f"模型加载完成! 输入尺寸: {detector.input_width()}x{detector.input_height()}")

    last_ms = time.time_ms()
    frame_count = 0

    while not app.need_exit():
        img = cam.read()
        if img is None:
            continue

        objs = detector.detect(img, conf_th=CONF_TH, iou_th=IOU_TH)

        if objs:
            print(f"[{frame_count:04d}] 检测到 {len(objs)} 个钢球:")
            for i, obj in enumerate(objs):
                cx = obj.x + obj.w // 2
                cy = obj.y + obj.h // 2
                print(f"  [{i}] 中心:({cx:4d},{cy:4d}) "
                      f"尺寸:{obj.w:3d}x{obj.h:3d} "
                      f"置信度:{obj.score:.3f}")

        img = draw_results(img, objs)

        img.draw_string(10, 10, f"Count: {len(objs)}",
                       color=image.Color.from_rgb(255, 255, 0), scale=2.0)

        frame_count += 1
        now = time.time_ms()
        fps = 1000.0 / max(now - last_ms, 1)
        last_ms = now
        img.draw_string(10, 42, f"FPS: {fps:.1f}",
                       color=image.Color.from_rgb(255, 255, 0), scale=1.2)

        disp.show(img)


if __name__ == "__main__":
    try:
        main()
    except Exception as e:
        print(f"错误: {e}")
        import traceback
        traceback.print_exc()
