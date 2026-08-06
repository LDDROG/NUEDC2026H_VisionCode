# LDD_ROG 2026.7.29

import argparse
import json
import shutil
import zipfile
from pathlib import Path

# 第一步，YOLOv11n.pt → ONNX

def export_onnx(weights_path, imgsz=224, output_dir="./output", opset=12):
    """使用 ultralytics 将 .pt 模型导出为 ONNX (FP32, 固定尺寸)"""
    from ultralytics import YOLO

    output_dir = Path(output_dir)
    output_dir.mkdir(parents=True, exist_ok=True)

    model = YOLO(weights_path)
    model_name = Path(weights_path).stem

    onnx_path = model.export(
        format="onnx",
        imgsz=imgsz,
        opset=opset,
        simplify=True,
        half=False,       # FP32，量化在后续cvimodel转换时做
        dynamic=False,    # 固定尺寸给MaixCam
        batch=1,
    )

    onnx_file = Path(onnx_path)
    dest = output_dir / f"{model_name}.onnx"
    if onnx_file != dest:
        shutil.move(str(onnx_file), str(dest))

    print(f"\nONNX 导出完成: {dest}")
    print(f"\n下一步: 将 ONNX 转为 cvimodel")
    print_tpu_mlir_guide(dest, imgsz)

    return dest

def print_tpu_mlir_guide(onnx_path, imgsz, mean=(0, 0, 0), std=(255, 255, 255)):
    s0 = 1.0 / std[0]
    s1 = 1.0 / std[1]
    s2 = 1.0 / std[2]
    m0, m1, m2 = mean



# 第二步，打包cviModel → MUD

def create_mud(cvimodel_path, labels, imgsz=224, output_dir="./output",
               model_name="yolo11n_maixcam", anchors=None,
               mean=0, std=255):
    cvimodel_path = Path(cvimodel_path)
    output_dir = Path(output_dir)
    output_dir.mkdir(parents=True, exist_ok=True)

    if not cvimodel_path.exists():
        print(f"错误: cvimodel文件不存在: {cvimodel_path}")
        return None

    # anchors和mean/std统一为列表格式
    if anchors is None:
        anchors = [10, 13, 16, 30, 33, 23, 30, 61, 62, 45,
                   59, 119, 116, 90, 156, 198, 373, 326]

    nc = len(labels)
    yolo_channels = (nc + 4) * 3  # 每个 scale: (x,y,w,h,obj,cls0,cls1,...) * 3 anchors

    # 构建report.json
    # 有些必须根据实际情况改，有些无所谓，同另一个代码
    report = {
        "anchors": anchors,
        "inputs": {"input0": [imgsz, imgsz, 3]},
        "outputs": {},
        "params_quantity": 0,
        "loss": [],
        "loss_conf": [],
        "loss_pos": [],
        "loss_class": [],
        "lr": [],
        "val_acc": [],
        "mean": mean if isinstance(mean, (int, float)) else mean[0],
        "std": std if isinstance(std, (int, float)) else std[0],
        "label_type": "detection",
        "data_type": "image",
        "labels": labels,
        "best_eval": {"epoch": 0, "val_acc": 0, "val_info": []},
    }

    # 计算三个检测头的输出尺寸
    strides = [8, 16, 32]
    for i, stride in enumerate(strides):
        grid = imgsz // stride
        report["outputs"][f"output{i}"] = [grid, grid, yolo_channels]

    mud_name = f"{model_name}.mud"
    mud_path = output_dir / mud_name

    with zipfile.ZipFile(str(mud_path), "w", zipfile.ZIP_DEFLATED) as zf:
        zf.write(cvimodel_path, cvimodel_path.name)
        zf.writestr("report.json", json.dumps(report, indent=2, ensure_ascii=False))

    print(f"\nMUD 文件已生成: {mud_path}")
    print(f"  类别: {labels}")
    print(f"  输入: {imgsz}x{imgsz}")
    print(f"  mean/std: {report['mean']}/{report['std']}")
    print(f"  输出: {[(k, v) for k, v in report['outputs'].items()]}")

    return mud_path


def try_read_meta(weights_path):
    meta_path = Path(weights_path).parent.parent / "maixcam_meta.json"
    if meta_path.exists():
        with open(meta_path, "r", encoding="utf-8") as f:
            return json.load(f)
    return None



def main():
    parser = argparse.ArgumentParser(description="YOLOv11n → MaixCam 格式导出")
    sub = parser.add_subparsers(dest="cmd", help="子命令")

    # ---- export: .pt → ONNX ----
    p_exp = sub.add_parser("export", help="导出 ONNX (YOLOv11n.pt → ONNX)")
    p_exp.add_argument("--weights", "-w", required=True, help="训练好的 .pt 模型路径")
    p_exp.add_argument("--imgsz", "-s", type=int, default=224, help="输入尺寸 (默认: 224)")
    p_exp.add_argument("--output", "-o", default="./output", help="输出目录")
    p_exp.add_argument("--opset", type=int, default=12, help="ONNX opset 版本 (默认: 12)")

    # ---- pack: CVIModel → MUD ----
    p_pack = sub.add_parser("pack", help="打包 CVIModel 为 .mud")
    p_pack.add_argument("--cvimodel", "-c", required=True, help="CVIModel 文件路径")
    p_pack.add_argument("--labels", "-l", nargs="+", required=True, help="类别名称列表")
    p_pack.add_argument("--imgsz", "-s", type=int, default=224, help="输入尺寸 (默认: 224)")
    p_pack.add_argument("--output", "-o", default="./output", help="输出目录")
    p_pack.add_argument("--name", "-n", default="yolo11n_maixcam", help="模型名称")
    p_pack.add_argument("--anchors", nargs="+", type=int, help="自定义 anchors (18个整数)")
    p_pack.add_argument("--mean", type=float, default=0, help="推理归一化均值 (默认: 0)")
    p_pack.add_argument("--std", type=float, default=255, help="推理归一化标准差 (默认: 255)")
    p_pack.add_argument("--meta", help="训练时生成的 maixcam_meta.json (自动读取 labels/imgsz)")

    # ---- guide: 仅打印 tpu-mlir 指南 ----
    p_guide = sub.add_parser("guide", help="打印 tpu-mlir 转换指南")
    p_guide.add_argument("--onnx", required=True, help="ONNX 文件路径")
    p_guide.add_argument("--imgsz", "-s", type=int, default=224, help="输入尺寸")
    p_guide.add_argument("--mean", type=float, nargs=3, default=[0, 0, 0], help="归一化均值")
    p_guide.add_argument("--std", type=float, nargs=3, default=[255, 255, 255], help="归一化标准差")

    args = parser.parse_args()

    if args.cmd == "export":
        # 尝试自动读取训练 meta
        meta = try_read_meta(args.weights)
        if meta:
            print(f"从 maixcam_meta.json 读取到: labels={meta.get('labels')}, imgsz={meta.get('imgsz')}")
        export_onnx(args.weights, args.imgsz, args.output, args.opset)

    elif args.cmd == "pack":
        # 尝试从 meta 文件读取 labels 和 imgsz
        labels = args.labels
        imgsz = args.imgsz
        if args.meta:
            meta_path = Path(args.meta)
            if meta_path.exists():
                with open(meta_path, "r", encoding="utf-8") as f:
                    meta = json.load(f)
                if not args.labels and "labels" in meta:
                    labels = meta["labels"]
                if "imgsz" in meta:
                    imgsz = meta["imgsz"]
                print(f"从 {args.meta} 读取: labels={labels}, imgsz={imgsz}")

        create_mud(args.cvimodel, labels, imgsz, args.output, args.name,
                   anchors=args.anchors, mean=args.mean, std=args.std)

    elif args.cmd == "guide":
        print_tpu_mlir_guide(Path(args.onnx), args.imgsz,
                            mean=tuple(args.mean), std=tuple(args.std))

    else:
        parser.print_help()
        print("\n完整流程示例:")
        print("  # 1. 导出 ONNX")
        print("  python export_to_maixcam.py export -w best.pt -s 224 -l ball circle")
        print()
        print("  # 2. 在线转换 (推荐): 上传 ONNX 到 https://maixhub.com/model-convert")
        print("  #    或本地转换: 查看 tpu-mlir 指南")
        print("  python export_to_maixcam.py guide --onnx output/best.onnx")
        print()
        print("  # 3. 打包 MUD")
        print("  python export_to_maixcam.py pack -c model.cvimodel -l ball circle -s 224")


if __name__ == "__main__":
    main()