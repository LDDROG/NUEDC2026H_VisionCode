# LDD_ROG 2026.7.29
# 给树莓派备用的

import argparse
import shutil
from pathlib import Path


def export_ncnn(weights_path, imgsz=224, output_dir="./output"):
    from ultralytics import YOLO

    output_dir = Path(output_dir)
    output_dir.mkdir(parents=True, exist_ok=True)

    model = YOLO(weights_path)
    model_name = Path(weights_path).stem

    print(f"正在导出 NCNN (imgsz={imgsz})...")
    ncnn_dir = model.export(format="ncnn", imgsz=imgsz, half=True)

    ncnn_dir = Path(ncnn_dir)
    dest_dir = output_dir / f"{model_name}_ncnn_model"
    if ncnn_dir != dest_dir:
        if dest_dir.exists():
            shutil.rmtree(dest_dir)
        shutil.move(str(ncnn_dir), str(dest_dir))

    print(f"NCNN 模型已导出: {dest_dir}")

    # 生成树莓派推理脚本
    generate_inference_script(output_dir, dest_dir.name, imgsz)

    print(f"\n{'='*50}")
    print("部署到树莓派:")
    print(f"  1. 把 {dest_dir.name}/ 整个文件夹复制到树莓派")
    print(f"  2. 把 run_ncnn.py 也复制过去")
    print(f"  3. 树莓派上安装依赖:")
    print(f"     pip install ultralytics opencv-python")
    print(f"  4. 运行:")
    print(f"     python run_ncnn.py --model {dest_dir.name} --source 0")
    print(f"{'='*50}")

    return dest_dir


def generate_inference_script(output_dir, model_folder, imgsz):
    script = f'''"""
树莓派 NCNN YOLO 推理脚本 — 支持 USB 摄像头 / 图片 / 视频。

用法:
  python run_ncnn.py --model {model_folder} --source 0          # USB摄像头
  python run_ncnn.py --model {model_folder} --source test.jpg   # 单张图片
  python run_ncnn.py --model {model_folder} --source video.mp4  # 视频文件
"""

import argparse
import time
from pathlib import Path

import cv2
import numpy as np
from ultralytics import YOLO


def main():
    parser = argparse.ArgumentParser(description="YOLOv11n NCNN 推理")
    parser.add_argument("--model", "-m", required=True, help="NCNN 模型文件夹路径")
    parser.add_argument("--source", "-s", default="0", help="输入源: 0=摄像头, 或图片/视频路径")
    parser.add_argument("--imgsz", type=int, default={imgsz}, help="输入分辨率")
    parser.add_argument("--conf", type=float, default=0.5, help="置信度阈值")
    parser.add_argument("--iou", type=float, default=0.45, help="NMS IoU 阈值")
    parser.add_argument("--save", action="store_true", help="保存推理结果图片/视频")
    parser.add_argument("--show-fps", action="store_true", default=True, help="显示 FPS")
    args = parser.parse_args()

    print(f"加载模型: {{args.model}}")
    model = YOLO(args.model, task="detect")

    # 解析输入源
    if args.source.isdigit():
        src = int(args.source)
        print(f"打开摄像头: {{src}}")
    else:
        src = args.source
        print(f"输入源: {{src}}")

    cap = None
    is_video = False

    if isinstance(src, int):
        cap = cv2.VideoCapture(src)
        cap.set(cv2.CAP_PROP_FRAME_WIDTH, 640)
        cap.set(cv2.CAP_PROP_FRAME_HEIGHT, 480)
        is_video = True
    elif Path(src).suffix.lower() in (".mp4", ".avi", ".mov", ".mkv"):
        cap = cv2.VideoCapture(src)
        is_video = True

    if is_video:
        fps_counter = []
        while True:
            ret, frame = cap.read()
            if not ret:
                break

            t0 = time.time()
            results = model(frame, imgsz=args.imgsz, conf=args.conf, iou=args.iou)
            t1 = time.time()

            fps = 1.0 / (t1 - t0) if (t1 - t0) > 0 else 0
            fps_counter.append(fps)

            annotated = results[0].plot()

            if args.show_fps:
                avg_fps = np.mean(fps_counter[-30:])
                cv2.putText(annotated, f"FPS: {{avg_fps:.1f}}",
                            (10, 30), cv2.FONT_HERSHEY_SIMPLEX, 1, (0, 255, 0), 2)
                cv2.putText(annotated, f"Infer: {{(t1-t0)*1000:.1f}}ms",
                            (10, 65), cv2.FONT_HERSHEY_SIMPLEX, 0.7, (0, 255, 0), 2)

            cv2.imshow("YOLOv11n NCNN", annotated)
            if cv2.waitKey(1) & 0xFF == ord("q"):
                break

        cap.release()
        cv2.destroyAllWindows()

        if fps_counter:
            print(f"平均 FPS: {{np.mean(fps_counter):.1f}}")

    else:
        # 单张图片
        t0 = time.time()
        results = model(src, imgsz=args.imgsz, conf=args.conf, iou=args.iou)
        t1 = time.time()

        annotated = results[0].plot()
        print(f"推理耗时: {{(t1-t0)*1000:.1f}}ms")
        print(f"检测到 {{len(results[0].boxes)}} 个目标")

        cv2.imshow("Result", annotated)
        print("按任意键关闭...")
        cv2.waitKey(0)
        cv2.destroyAllWindows()

        if args.save:
            out_path = Path(src).stem + "_result.jpg"
            cv2.imwrite(out_path, annotated)
            print(f"已保存: {{out_path}}")


if __name__ == "__main__":
    main()
'''

    out_path = Path(output_dir) / "run_ncnn.py"
    out_path.write_text(script, encoding="utf-8")
    print(f"推理脚本已生成: {out_path}")


def main():
    parser = argparse.ArgumentParser(description="YOLOv11n → NCNN 导出 (树莓派部署)")
    parser.add_argument("--weights", "-w", required=True, help="训练好的 .pt 模型")
    parser.add_argument("--imgsz", "-s", type=int, default=224, help="输入尺寸 (默认: 224)")
    parser.add_argument("--output", "-o", default="./output", help="输出目录")
    args = parser.parse_args()

    export_ncnn(args.weights, args.imgsz, args.output)


if __name__ == "__main__":
    main()
