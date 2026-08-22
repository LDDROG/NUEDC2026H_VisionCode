"""
钢球 YOLO 检测 + 实时计数
===========================
接入你自己的钢球权重后，只需改两处：
  1. MODEL_PATH — 模型文件路径（.kmodel / .mud）
  2. parse_yolo_output() — 根据你的模型输出格式解析检测框
"""
from maix import camera, display, image, app, time, touchscreen, nn
from maix._maix.image import cv2image
import numpy as np
import cv2

# ======================== 硬件初始化 ========================
CAM_W = 512
CAM_H = 320
cam = camera.Camera(CAM_W, CAM_H)
disp = display.Display()
ts = touchscreen.TouchScreen()

EXIT_X, EXIT_Y, EXIT_W, EXIT_H = CAM_W - 80, 0, 80, 40

# ======================== 模型配置 ========================
MODEL_PATH = "iron_ball.kmodel"     # TODO: 替换为你的钢球权重路径
INPUT_W = 320                        # 模型输入宽度
INPUT_H = 320                        # 模型输入高度
CONF_THRESH = 0.5                    # 置信度阈值
NMS_THRESH = 0.45                    # NMS IoU 阈值
MAX_DET = 50                         # 单帧最大检测数


# ======================== 模型加载 ========================
# TODO: 根据你的模型类型选择加载方式
# model = nn.NN(MODEL_PATH)           # .kmodel
# model = nn.load(MODEL_PATH)         # 其他格式


def preprocess(img_cv):
    """预处理：缩放到模型输入尺寸 + 归一化"""
    img = cv2.resize(img_cv, (INPUT_W, INPUT_H))
    # 如果你训练的模型需要归一化到 0~1，取消下面这行注释
    # img = img.astype(np.float32) / 255.0
    # 如果你训练的模型需要 BGR→RGB，取消下面这行注释
    # img = cv2.cvtColor(img, cv2.COLOR_BGR2RGB)
    return img


def run_inference(input_img):
    """
    模型推理
    TODO: 根据 nn 模块实际 API 调整
    """
    # model.forward(input_img)
    # output = model.get_outputs()
    # return output
    raise NotImplementedError("请接入 YOLO 模型后实现此函数")


def parse_yolo_output(output):
    """
    解析 YOLO 输出 → 检测框列表

    TODO: 根据你的模型输出格式实现解析逻辑。
    常见 YOLO 输出格式举例：

    格式A — [1, 84, 8400] (YOLOv8): 前4通道=cx/cy/w/h, 后80通道=类别分数
    格式B — [1, 5+N, num_boxes] (YOLOv5): x/y/w/h/conf + class_probs

    返回值: [(x1, y1, x2, y2, conf, class_id), ...] 坐标在原图尺寸下
    """
    raise NotImplementedError("请根据模型输出格式实现解析逻辑")


def nms(boxes, iou_thresh=NMS_THRESH):
    """非极大值抑制，去重框"""
    if len(boxes) == 0:
        return []
    boxes = sorted(boxes, key=lambda b: b[4], reverse=True)
    keep = []
    while boxes:
        best = boxes.pop(0)
        keep.append(best)
        boxes = [
            b for b in boxes
            if box_iou(best, b) < iou_thresh
        ]
    return keep


def box_iou(a, b):
    """计算两个框的 IoU"""
    x1 = max(a[0], b[0])
    y1 = max(a[1], b[1])
    x2 = min(a[2], b[2])
    y2 = min(a[3], b[3])
    inter = max(0, x2 - x1) * max(0, y2 - y1)
    area_a = (a[2] - a[0]) * (a[3] - a[1])
    area_b = (b[2] - b[0]) * (b[3] - b[1])
    return inter / (area_a + area_b - inter + 1e-6)


def find_iron_ball():
    img = cam.read()
    img_cv = image.image2cv(img, False, False)

    # ---------- 推理 ----------
    input_img = preprocess(img_cv)
    try:
        raw = run_inference(input_img)
        detections = parse_yolo_output(raw)
    except NotImplementedError:
        detections = []

    # ---------- NMS + 过滤 ----------
    valid = [d for d in detections if d[4] >= CONF_THRESH]
    valid = nms(valid)[:MAX_DET]

    # ---------- 画框 ----------
    count = len(valid)
    for x1, y1, x2, y2, conf, cls_id in valid:
        cv2.rectangle(img_cv, (int(x1), int(y1)), (int(x2), int(y2)), (0, 255, 0), 2)
        cv2.putText(img_cv, f"{conf:.2f}", (int(x1), int(y1) - 4),
                    cv2.FONT_HERSHEY_SIMPLEX, 0.5, (0, 255, 0), 1)

    # ---------- 显示 ----------
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
