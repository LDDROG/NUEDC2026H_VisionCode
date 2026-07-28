from maix import camera, display, image, app, time, touchscreen
from maix._maix.image import cv2image
import cv2
import numpy as np

CAM_W = 512
CAM_H = 320
SCALE = 0.5           # 检测用缩放比例，缩小大幅提速
DET_W = int(CAM_W * SCALE)
DET_H = int(CAM_H * SCALE)

cam = camera.Camera(CAM_W, CAM_H)
disp = display.Display()
ts = touchscreen.TouchScreen()

EXIT_X, EXIT_Y, EXIT_W, EXIT_H = CAM_W - 80, 0, 80, 40

# ========== 检测参数 ==========
CANNY_LOW = 50
CANNY_HIGH = 150
MIN_CIRCULARITY = 0.7    # 圆形度阈值，越高越严格
MIN_AREA = 20            # 最小轮廓面积（检测分辨率下）
MAX_AREA = DET_W * DET_H * 0.6


def find_iron_ball():
    img = cam.read()
    img_cv = image.image2cv(img, False, False)

    # 缩小图像做检测，大幅减少计算量
    small = cv2.resize(img_cv, (DET_W, DET_H))
    gray = cv2.cvtColor(small, cv2.COLOR_RGB2GRAY)
    blur = cv2.GaussianBlur(gray, (3, 3), 0)
    edges = cv2.Canny(blur, CANNY_LOW, CANNY_HIGH)

    contours, _ = cv2.findContours(edges, cv2.RETR_EXTERNAL, cv2.CHAIN_APPROX_SIMPLE)

    count = 0
    for cnt in contours:
        area = cv2.contourArea(cnt)
        if area < MIN_AREA or area > MAX_AREA:
            continue

        perimeter = cv2.arcLength(cnt, True)
        if perimeter == 0:
            continue
        if 4 * np.pi * area / (perimeter * perimeter) < MIN_CIRCULARITY:
            continue

        (x, y), r = cv2.minEnclosingCircle(cnt)
        circle_area = np.pi * r * r
        if circle_area > 0 and area / circle_area < 0.4:
            continue

        # 坐标还原到原图尺寸
        x, y, r = int(x / SCALE), int(y / SCALE), int(r / SCALE)
        cv2.circle(img_cv, (x, y), r, (0, 255, 0), 2)
        cv2.circle(img_cv, (x, y), 2, (0, 0, 255), 3)
        count += 1

    img_maix = cv2image(img_cv, False, False)
    fps = time.fps()
    img_out = img_maix.draw_string(0, 0, f"FPS:{fps:.1f}", color=image.COLOR_RED)
    img_out.draw_string(0, 20, f"Count:{count}", color=image.COLOR_GREEN)
    img_out.draw_rect(EXIT_X, EXIT_Y, EXIT_W, EXIT_H, color=image.COLOR_RED, thickness=2)
    img_out.draw_string(EXIT_X + 14, EXIT_Y + 10, "EXIT", color=image.COLOR_RED)
    disp.show(img_out)


def check_exit():
    if app.need_exit():
        return True
    touched, x, y = ts.read()
    if touched and EXIT_X <= x <= EXIT_X + EXIT_W and EXIT_Y <= y <= EXIT_Y + EXIT_H:
        return True
    return False


def run():
    while not check_exit():
        find_iron_ball()
