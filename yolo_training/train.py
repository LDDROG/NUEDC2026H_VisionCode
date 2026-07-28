"""
YOLOv11n 训练脚本 — 针对 MaixCam 平台优化。

MaixCam 推荐输入尺寸:
  224x224  — 速度最快, 推理约 8-15ms, 适合简单/中距离目标
  320x320  — 速度和精度平衡
  448x448  — 精度更高, 但推理较慢

用法:
  python train.py --data ./dataset_yolo/dataset.yaml --weights ./weights/yolo11n.pt
  python train.py --data ./dataset_yolo/dataset.yaml -s 224 -e 100 -b 32 --balance
"""

import argparse
import json
import random
from pathlib import Path

import numpy as np
import yaml
from ultralytics import YOLO


# ============================================================
# 数据均衡: 对少数类样本进行过采样
# ============================================================

def balance_dataset(label_dir, img_dir, class_names, threshold=1.5):
    """分析各类别样本分布, 返回过采样后的 (image_path, label_path) 列表。

    当 多数类样本数 / 少数类样本数 > threshold 时,
    对包含少数类的图片进行重复采样, 使各类别趋于均衡。
    """
    label_dir = Path(label_dir)
    img_dir = Path(img_dir)

    label_files = sorted(label_dir.glob("*.txt"))
    if not label_files:
        return None

    nc = len(class_names)
    class_to_images = {i: set() for i in range(nc)}
    img_label_pairs = []

    for lf in label_files:
        content = lf.read_text(encoding="utf-8").strip()
        img_path = None
        for ext in (".jpg", ".jpeg", ".png", ".bmp"):
            candidate = img_dir / f"{lf.stem}{ext}"
            if candidate.exists():
                img_path = candidate
                break
        if img_path is None:
            continue

        img_label_pairs.append((img_path, lf))
        if content:
            for line in content.splitlines():
                try:
                    cls_id = int(line.split()[0])
                    if cls_id < nc:
                        class_to_images[cls_id].add(len(img_label_pairs) - 1)
                except (ValueError, IndexError):
                    pass

    if nc <= 1:
        return None  # 单类别不需要均衡

    counts = [len(class_to_images[i]) for i in range(nc)]
    max_count = max(counts) if counts else 1

    print(f"各类别覆盖图片数: {dict(zip(class_names, counts))}")

    balanced = list(img_label_pairs)
    for cls_id in range(nc):
        if counts[cls_id] == 0:
            continue
        ratio = max_count / max(counts[cls_id], 1)
        if ratio > threshold:
            extra_needed = int(len(class_to_images[cls_id]) * (ratio - 1))
            candidates = list(class_to_images[cls_id])
            for _ in range(extra_needed):
                idx = random.choice(candidates)
                balanced.append(img_label_pairs[idx])
            print(f"  {class_names[cls_id]}: {counts[cls_id]} → {counts[cls_id] + extra_needed} (过采样 {extra_needed} 张)")

    random.shuffle(balanced)
    return balanced


def create_balanced_yaml(original_yaml_path, balanced_pairs, output_path):
    """根据过采样后的图片列表, 创建临时目录和 dataset.yaml"""
    import shutil
    output_path = Path(output_path)
    (output_path / "images").mkdir(parents=True, exist_ok=True)
    (output_path / "labels").mkdir(parents=True, exist_ok=True)

    for img_path, label_path in balanced_pairs:
        dest_img = output_path / "images" / img_path.name
        dest_label = output_path / "labels" / label_path.name
        if not dest_img.exists():
            shutil.copy2(img_path, dest_img)
        if not dest_label.exists():
            shutil.copy2(label_path, dest_label)

    with open(original_yaml_path, "r", encoding="utf-8") as f:
        cfg = yaml.safe_load(f)

    cfg["path"] = str(output_path.resolve())
    cfg["train"] = "images"
    cfg["val"] = cfg.get("val", "images")

    yaml_path = output_path / "dataset.yaml"
    with open(yaml_path, "w", encoding="utf-8") as f:
        yaml.dump(cfg, f, allow_unicode=True, default_flow_style=False)

    return yaml_path


# ============================================================
# 主训练入口
# ============================================================

def main():
    parser = argparse.ArgumentParser(description="YOLOv11n 训练 for MaixCam")

    # ---- 基础参数 (与 MaixHub 训练页面对齐) ----
    parser.add_argument("--data", "-d", required=True, help="dataset.yaml 路径")
    parser.add_argument("--weights", "-w", default="yolo11n.pt", help="预训练权重路径 (默认: yolo11n.pt)")
    parser.add_argument("--imgsz", "-s", type=int, default=224, help="输入分辨率 (默认: 224)")
    parser.add_argument("--epochs", "-e", type=int, default=100, help="训练轮数 (默认: 100)")
    parser.add_argument("--batch", "-b", type=int, default=32, help="批次大小 (默认: 32)")
    parser.add_argument("--lr", type=float, default=0.001, help="初始学习率 (默认: 0.001)")
    parser.add_argument("--name", "-n", default="yolo11n_maixcam", help="实验名称")
    parser.add_argument("--device", default="cuda", help="训练设备: cuda, cpu, mps")
    parser.add_argument("--workers", type=int, default=4, help="数据加载线程数")

    # ---- 优化器 & 学习率 ----
    parser.add_argument("--optimizer", default="AdamW", help="优化器: AdamW, SGD, Adam")
    parser.add_argument("--weight-decay", type=float, default=0.0005, help="权重衰减")
    parser.add_argument("--warmup-epochs", type=int, default=3, help="热身轮数")
    parser.add_argument("--cos-lr", action="store_true", default=True, help="余弦学习率衰减 (默认开启)")

    # ---- 早停 & 恢复 ----
    parser.add_argument("--patience", type=int, default=50, help="早停耐心值: 连续N轮无提升则停止 (默认: 50)")
    parser.add_argument("--resume", action="store_true", help="从上次中断处恢复训练")

    # ================================================================
    # ---- 数据增强 (对应 MaixHub 训练选项) ----
    # ================================================================

    aug = parser.add_argument_group("数据增强 (对应 MaixHub 训练页选项)")
    aug.add_argument("--fliplr", type=float, default=0.0,
                     help="随机水平镜像概率 (默认: 0=禁用)")
    aug.add_argument("--flipud", type=float, default=0.0,
                     help="随机垂直翻转概率 (默认: 0, 一般物体检测不需要)")
    aug.add_argument("--degrees", type=float, default=10.0,
                     help="随机旋转角度范围 (默认: 10°, 0=禁用)")
    aug.add_argument("--translate", type=float, default=0.1,
                     help="随机平移比例 (默认: 0.1)")
    aug.add_argument("--scale", type=float, default=0.5,
                     help="随机缩放比例 (默认: 0.5, 即 50%%~150%% 缩放)")
    aug.add_argument("--shear", type=float, default=2.0,
                     help="随机错切角度 (默认: 2°, 0=禁用)")
    aug.add_argument("--perspective", type=float, default=0.0,
                     help="随机透视变换概率 (默认: 0, 一般关闭)")

    aug.add_argument("--hsv-h", type=float, default=0.015,
                     help="HSV 色调扰动 (默认: 0.015)")
    aug.add_argument("--hsv-s", type=float, default=0.7,
                     help="HSV 饱和度扰动 (默认: 0.7)")
    aug.add_argument("--hsv-v", type=float, default=0.4,
                     help="HSV 明度扰动 (默认: 0.4)")

    aug.add_argument("--mosaic", type=float, default=1.0,
                     help="Mosaic 增强概率 (默认: 1.0, 小数据集建议开启)")
    aug.add_argument("--mixup", type=float, default=0.1,
                     help="MixUp 增强概率 (默认: 0.1, 0=禁用)")
    aug.add_argument("--close-mosaic", type=int, default=15,
                     help="最后 N 轮关闭 mosaic (默认: 15, 让模型适应真实尺度)")

    aug.add_argument("--copy-paste", type=float, default=0.0,
                     help="Copy-Paste 增强概率 (默认: 0, 小目标检测可开启)")

    # ---- 数据均衡 (对应 MaixHub "数据均衡" 选项) ----
    bal = parser.add_argument_group("数据均衡 (对应 MaixHub '启动数据均衡' 选项)")
    bal.add_argument("--balance", action="store_true",
                     help="启用数据均衡: 对少数类样本过采样, 防止模型偏向多数类")
    bal.add_argument("--balance-thresh", type=float, default=1.5,
                     help="均衡触发阈值: 多数/少数比例 > 此值才触发 (默认: 1.5)")

    args = parser.parse_args()

    # ---- 检查数据集 ----
    data_path = Path(args.data)
    if not data_path.exists():
        print(f"错误: 数据集配置 {args.data} 不存在!")
        print("请先运行 prepare_dataset.py 转换数据集格式")
        return

    # ---- 读取类别信息 ----
    with open(data_path, "r", encoding="utf-8") as f:
        cfg = yaml.safe_load(f)
    names = cfg.get("names", {})
    if isinstance(names, dict):
        class_names = [names[i] for i in range(len(names))]
    else:
        class_names = list(names) if names else []

    # ---- 数据均衡 ----
    actual_data = str(data_path.resolve())
    if args.balance:
        print("正在分析数据分布...")
        data_dir = Path(cfg.get("path", "."))
        if not data_dir.is_absolute():
            data_dir = data_path.parent / data_dir
        img_dir = data_dir / "images"
        label_dir = data_dir / "labels"

        balanced_pairs = balance_dataset(label_dir, img_dir, class_names, args.balance_thresh)
        if balanced_pairs:
            balanced_dir = Path("./dataset_balanced")
            balanced_yaml = create_balanced_yaml(data_path, balanced_pairs, balanced_dir)
            actual_data = str(balanced_yaml.resolve())
            print(f"均衡后数据集已生成: {actual_data}")
        else:
            print("单类别或分布已均衡, 跳过数据均衡")

    # ---- 加载模型 ----
    if Path(args.weights).exists():
        print(f"从本地加载权重: {args.weights}")
        model = YOLO(args.weights)
    else:
        print("下载 YOLOv11n 预训练权重...")
        model = YOLO("yolo11n.pt")

    print(f"\n训练配置:")
    print(f"  数据集: {actual_data}")
    print(f"  类别: {class_names} ({len(class_names)} 类)")
    print(f"  输入尺寸: {args.imgsz}x{args.imgsz}")
    print(f"  训练轮数: {args.epochs}")
    print(f"  批次大小: {args.batch}")
    print(f"  学习率: {args.lr}")
    print(f"  设备: {args.device}")
    print(f"  水平镜像: {args.fliplr}")
    print(f"  旋转角度: {args.degrees}°")
    print(f"  Mosaic: {args.mosaic}")
    print(f"  MixUp: {args.mixup}")
    print()

    # ---- 训练 ----
    results = model.train(
        data=actual_data,
        epochs=args.epochs,
        imgsz=args.imgsz,
        batch=args.batch,
        lr0=args.lr,
        name=args.name,
        device=args.device,
        resume=args.resume,
        patience=args.patience,
        workers=args.workers,
        optimizer=args.optimizer,
        cos_lr=args.cos_lr,
        close_mosaic=args.close_mosaic,
        weight_decay=args.weight_decay,
        warmup_epochs=args.warmup_epochs,
        # 数据增强
        hsv_h=args.hsv_h,
        hsv_s=args.hsv_s,
        hsv_v=args.hsv_v,
        degrees=args.degrees,
        translate=args.translate,
        scale=args.scale,
        shear=args.shear,
        perspective=args.perspective,
        flipud=args.flipud,
        fliplr=args.fliplr,
        mosaic=args.mosaic,
        mixup=args.mixup,
        copy_paste=args.copy_paste,
    )

    # ---- 保存 MaixCam 配置 ----
    best_pt = Path(results.save_dir) / "weights" / "best.pt"
    meta = {
        "imgsz": args.imgsz,
        "labels": class_names,
        "nc": len(class_names),
    }
    with open(Path(results.save_dir) / "maixcam_meta.json", "w", encoding="utf-8") as f:
        json.dump(meta, f, indent=2, ensure_ascii=False)

    print(f"\n{'='*50}")
    print(f"训练完成! 最佳模型: {best_pt}")
    print(f"类别: {class_names}")
    print(f"\n下一步: python export_to_maixcam.py export -w {best_pt} -s {args.imgsz} -l {' '.join(class_names)}")
    print(f"{'='*50}")


if __name__ == "__main__":
    main()
