# LDD_ROG 2026.7.29
# 验证数据集完整性

import argparse
from pathlib import Path
from collections import Counter


def main():
    parser = argparse.ArgumentParser(description="验证 YOLO 数据集")
    parser.add_argument("--data", "-d", required=True, help="dataset.yaml 路径")
    args = parser.parse_args()

    import yaml

    data_path = Path(args.data)
    if not data_path.exists():
        print(f"错误: {args.data} 不存在")
        return

    with open(data_path, "r", encoding="utf-8") as f:
        cfg = yaml.safe_load(f)

    nc = cfg.get("nc", 0)
    names = cfg.get("names", {})

    print(f"类别数: {nc}")
    print(f"类别名: {names}")
    print()

    # 遍历图片和标注
    data_dir = Path(cfg.get("path", "."))
    if not data_dir.is_absolute():
        data_dir = data_path.parent / data_dir

    total_imgs = 0
    total_labels = 0
    class_counts = Counter()
    missing_labels = []
    empty_labels = []
    bad_labels = []

    img_dirs = []
    for key in ("train", "val", "test"):
        p = data_dir / cfg.get(key, "")
        if p.exists():
            img_dirs.append((key, p))

    if not img_dirs:
        # fallback: images 直接在 data_dir 下
        img_dir = data_dir / "images"
        if img_dir.exists():
            img_dirs.append(("images", img_dir))

    for split, img_dir in img_dirs:
        label_dir = data_dir / "labels"
        if not label_dir.exists():
            label_dir = img_dir.parent / "labels"

        imgs = list(img_dir.glob("*.jpg")) + list(img_dir.glob("*.png"))
        print(f"[{split}] 图片: {len(imgs)} 张")

        for img in imgs:
            total_imgs += 1
            label_file = label_dir / f"{img.stem}.txt"
            if not label_file.exists():
                missing_labels.append(img.name)
                continue

            content = label_file.read_text(encoding="utf-8").strip()
            if not content:
                empty_labels.append(img.name)
                continue

            for line in content.splitlines():
                parts = line.strip().split()
                if len(parts) != 5:
                    bad_labels.append((img.name, line))
                    continue
                cls_id = int(parts[0])
                total_labels += 1
                class_counts[cls_id] += 1

                # 检查坐标归一化
                cx, cy, w, h = map(float, parts[1:])
                if not (0 <= cx <= 1 and 0 <= cy <= 1 and 0 < w <= 1 and 0 < h <= 1):
                    bad_labels.append((img.name, f"坐标越界: {line}"))

    print(f"\n========== 验证报告 ==========")
    print(f"总图片数: {total_imgs}")
    print(f"总标注框: {total_labels}")
    print()

    if missing_labels:
        print(f"缺少标注文件: {len(missing_labels)} 张")
        for n in missing_labels[:10]:
            print(f"  - {n}")
        if len(missing_labels) > 10:
            print(f"  ... 还有 {len(missing_labels) - 10} 张")
    else:
        print("所有图片都有对应标注文件 ✓")

    if empty_labels:
        print(f"\n空标注 (负样本): {len(empty_labels)} 张")
    else:
        print("无空标注文件 ✓")

    if bad_labels:
        print(f"\n格式错误的标注: {len(bad_labels)} 个")
        for img, line in bad_labels[:5]:
            print(f"  - {img}: {line}")
    else:
        print("标注格式全部正确 ✓")

    print(f"\n各类别框数分布:")
    for cls_id in sorted(class_counts.keys()):
        cls_name = names.get(cls_id, f"class_{cls_id}")
        print(f"  {cls_name} (id={cls_id}): {class_counts[cls_id]}")

    if class_counts:
        min_cls = min(class_counts.values())
        max_cls = max(class_counts.values())
        if max_cls / max(min_cls, 1) > 5:
            print("\n⚠ 警告: 类别样本数差异较大, 建议补充数据或使用类别权重")


if __name__ == "__main__":
    main()