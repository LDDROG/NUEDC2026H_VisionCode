"""
将 MaixHub 下载的数据集转换为 YOLO 训练格式。

MaixHub 数据集常见格式:
  1. 图片 + VOC XML 标注
  2. 图片 + COCO JSON 标注
  3. 图片 + MaixHub JSON 标注 (每个图片一个json)

用法:
  # 基础转换
  python prepare_dataset.py -i ./dataset_raw -o ./dataset_yolo -l ball circle

  # 启用最小框过滤 + 负样本保留 + 训练/验证集划分
  python prepare_dataset.py -i ./dataset_raw -o ./dataset_yolo -l ball \\
      --min-bbox 10 --keep-negatives --split 0.8
"""

import argparse
import json
import random
import shutil
import xml.etree.ElementTree as ET
from pathlib import Path


def filter_small_boxes(lines, min_pixels, img_w, img_h):
    """过滤像素大小低于 min_pixels 的标注框。

    lines: YOLO 格式的标注行 ["cls cx cy w h", ...]
    img_w, img_h: 图片宽高
    min_pixels: 最小像素阈值 (框的最短边)
    """
    if min_pixels <= 0:
        return lines

    filtered = []
    for line in lines:
        parts = line.strip().split()
        if len(parts) != 5:
            continue
        _, _, _, w, h = parts
        pw = float(w) * img_w   # 框的实际像素宽
        ph = float(h) * img_h   # 框的实际像素高
        if min(pw, ph) >= min_pixels:
            filtered.append(line)
        else:
            print(f"  过滤小框: {pw:.0f}x{ph:.0f}px < {min_pixels}px")

    return filtered


def voc_xml_to_yolo(xml_path, img_w, img_h, class_map, min_bbox=0):
    """将 VOC XML 标注转为 YOLO 格式: class_id cx cy w h (归一化)"""
    tree = ET.parse(xml_path)
    root = tree.getroot()
    labels = []
    for obj in root.findall("object"):
        cls_name = obj.find("name").text
        if cls_name not in class_map:
            continue
        cls_id = class_map[cls_name]
        bbox = obj.find("bndbox")
        xmin = float(bbox.find("xmin").text)
        ymin = float(bbox.find("ymin").text)
        xmax = float(bbox.find("xmax").text)
        ymax = float(bbox.find("ymax").text)
        labels.append(f"{cls_id} {(xmin + xmax) / 2 / img_w:.6f} "
                      f"{(ymin + ymax) / 2 / img_h:.6f} "
                      f"{(xmax - xmin) / img_w:.6f} "
                      f"{(ymax - ymin) / img_h:.6f}")

    return filter_small_boxes(labels, min_bbox, img_w, img_h)


def coco_json_to_yolo(coco_path, img_dir, out_dir, class_map, min_bbox=0, keep_negatives=True):
    """将 COCO JSON 标注转为 YOLO 格式"""
    with open(coco_path, "r", encoding="utf-8") as f:
        data = json.load(f)

    categories = {cat["id"]: cat["name"] for cat in data["categories"]}
    anns_by_image = {}
    for ann in data["annotations"]:
        img_id = ann["image_id"]
        anns_by_image.setdefault(img_id, []).append(ann)

    (out_dir / "labels").mkdir(parents=True, exist_ok=True)
    (out_dir / "images").mkdir(parents=True, exist_ok=True)

    count = 0
    for img in data["images"]:
        img_name = Path(img["file_name"]).stem
        img_w, img_h = img["width"], img["height"]
        anns = anns_by_image.get(img["id"], [])

        lines = []
        for ann in anns:
            cls_name = categories.get(ann["category_id"])
            if cls_name not in class_map:
                continue
            cls_id = class_map[cls_name]
            x, y, w, h = ann["bbox"]
            lines.append(f"{cls_id} {(x + w / 2) / img_w:.6f} "
                         f"{(y + h / 2) / img_h:.6f} {w / img_w:.6f} {h / img_h:.6f}")

        lines = filter_small_boxes(lines, min_bbox, img_w, img_h)

        if not lines and not keep_negatives:
            continue  # 跳过无标注的图片

        label_path = out_dir / "labels" / f"{img_name}.txt"
        label_path.write_text("\n".join(lines), encoding="utf-8")

        src_img = Path(img_dir) / img["file_name"]
        if src_img.exists():
            shutil.copy2(src_img, out_dir / "images" / img["file_name"])
            count += 1

    return count


def maixhub_json_to_yolo(json_dir, img_dir, out_dir, class_map, min_bbox=0, keep_negatives=True):
    """将 MaixHub JSON 标注转为 YOLO 格式。"""
    (out_dir / "labels").mkdir(parents=True, exist_ok=True)
    (out_dir / "images").mkdir(parents=True, exist_ok=True)

    json_files = list(Path(json_dir).glob("*.json"))
    count = 0

    for jf in json_files:
        with open(jf, "r", encoding="utf-8") as f:
            data = json.load(f)

        if isinstance(data, list):
            for item in data:
                count += _process_maixhub_item(item, img_dir, out_dir, class_map,
                                               min_bbox, keep_negatives)
        elif isinstance(data, dict) and "bbox" in data:
            count += _process_maixhub_item(data, img_dir, out_dir, class_map,
                                           min_bbox, keep_negatives)

    return count


def _process_maixhub_item(item, img_dir, out_dir, class_map, min_bbox=0, keep_negatives=True):
    name = item.get("name") or item.get("file_name") or item.get("image")
    if not name:
        return 0

    img_name = Path(name).stem
    img_w = item.get("width", 640)
    img_h = item.get("height", 640)

    bboxes = item.get("bbox", []) or item.get("boxes", [])
    lines = []
    for b in bboxes:
        if len(b) == 5:
            x1, y1, x2, y2, label = b
        elif len(b) == 6:
            x1, y1, x2, y2, _, label = b
        else:
            continue

        label_str = str(label)
        if label_str.isdigit():
            cls_id = int(label_str)
        else:
            if label_str not in class_map:
                class_map[label_str] = len(class_map)
            cls_id = class_map[label_str]

        lines.append(f"{cls_id} {(x1 + x2) / 2 / img_w:.6f} "
                     f"{(y1 + y2) / 2 / img_h:.6f} "
                     f"{(x2 - x1) / img_w:.6f} "
                     f"{(y2 - y1) / img_h:.6f}")

    lines = filter_small_boxes(lines, min_bbox, img_w, img_h)

    if not lines and not keep_negatives:
        return 0

    label_path = out_dir / "labels" / f"{img_name}.txt"
    label_path.write_text("\n".join(lines), encoding="utf-8")

    for ext in [".jpg", ".jpeg", ".png", ".bmp"]:
        src = Path(img_dir) / f"{img_name}{ext}"
        if src.exists():
            shutil.copy2(src, out_dir / "images" / src.name)
            return 1
        src = Path(img_dir) / name
        if src.exists():
            shutil.copy2(src, out_dir / "images" / name)
            return 1

    return 0


def split_train_val(output_dir, train_ratio=0.8, seed=42):
    """将 images/ 和 labels/ 拆分为 train/ 和 val/ 子目录。

    输出结构:
      output_dir/
        train/images/  train/labels/
        val/images/    val/labels/
    """
    output_dir = Path(output_dir)
    img_dir = output_dir / "images"
    label_dir = output_dir / "labels"
    if not img_dir.exists() or not label_dir.exists():
        return

    images = sorted(img_dir.glob("*"))
    if not images:
        return

    random.seed(seed)
    random.shuffle(images)
    split_idx = int(len(images) * train_ratio)
    train_imgs = images[:split_idx]
    val_imgs = images[split_idx:]

    for subset, img_list in [("train", train_imgs), ("val", val_imgs)]:
        (output_dir / subset / "images").mkdir(parents=True, exist_ok=True)
        (output_dir / subset / "labels").mkdir(parents=True, exist_ok=True)
        for img in img_list:
            shutil.move(str(img), str(output_dir / subset / "images" / img.name))
            label = label_dir / f"{img.stem}.txt"
            if label.exists():
                shutil.move(str(label), str(output_dir / subset / "labels" / label.name))

    # 清理空的原目录
    for d in [img_dir, label_dir]:
        if d.exists() and not any(d.iterdir()):
            d.rmdir()

    print(f"训练集: {len(train_imgs)} 张, 验证集: {len(val_imgs)} 张")


def auto_detect_and_convert(input_dir, output_dir, class_map=None,
                            min_bbox=0, keep_negatives=True, split=None):
    """自动检测数据集格式并转换"""
    input_dir = Path(input_dir)
    output_dir = Path(output_dir)

    if class_map is None:
        class_map = {}

    xml_files = list(input_dir.glob("*.xml"))
    json_files = list(input_dir.glob("*.json"))
    jpg_files = list(input_dir.glob("*.jpg")) + list(input_dir.glob("*.png"))

    print(f"发现: {len(xml_files)} XML, {len(json_files)} JSON, {len(jpg_files)} 图片")

    (output_dir / "images").mkdir(parents=True, exist_ok=True)
    (output_dir / "labels").mkdir(parents=True, exist_ok=True)

    count = 0
    neg_count = 0

    if xml_files:
        print("检测到 VOC XML 格式, 正在转换...")
        for xml_path in xml_files:
            img_name = xml_path.stem
            img_path = None
            for ext in [".jpg", ".jpeg", ".png", ".bmp"]:
                candidate = input_dir / f"{img_name}{ext}"
                if candidate.exists():
                    img_path = candidate
                    break

            if img_path is None:
                print(f"  警告: 找不到 {img_name} 图片, 跳过")
                continue

            tree = ET.parse(xml_path)
            root = tree.getroot()
            size = root.find("size")
            img_w = int(size.find("width").text) if size is not None else 640
            img_h = int(size.find("height").text) if size is not None else 640

            lines = voc_xml_to_yolo(xml_path, img_w, img_h, class_map, min_bbox)

            if not lines and not keep_negatives:
                continue
            if not lines:
                neg_count += 1

            label_path = output_dir / "labels" / f"{img_name}.txt"
            label_path.write_text("\n".join(lines), encoding="utf-8")
            shutil.copy2(img_path, output_dir / "images" / img_path.name)
            count += 1
        print(f"  转换: {count} 张 (含 {neg_count} 张负样本)")

    elif json_files:
        for jf in json_files:
            with open(jf, "r", encoding="utf-8") as f:
                data = json.load(f)
            if isinstance(data, dict) and "images" in data and "annotations" in data:
                print("检测到 COCO JSON 格式, 正在转换...")
                count = coco_json_to_yolo(jf, input_dir, output_dir, class_map,
                                          min_bbox, keep_negatives)
                print(f"  转换: {count} 张")
                break
        else:
            print("检测到 JSON 标注, 尝试 MaixHub 格式...")
            count = maixhub_json_to_yolo(input_dir, input_dir, output_dir, class_map,
                                         min_bbox, keep_negatives)
            print(f"  转换: {count} 张")

    # 处理纯图片文件 (无标注 = 负样本)
    unlabeled_imgs = []
    for ext in [".jpg", ".jpeg", ".png", ".bmp"]:
        for img in input_dir.glob(f"*{ext}"):
            label = output_dir / "labels" / f"{img.stem}.txt"
            if not label.exists():
                unlabeled_imgs.append(img)

    if unlabeled_imgs and keep_negatives:
        print(f"\n发现 {len(unlabeled_imgs)} 张无标注图片, 作为负样本保留")
        for img in unlabeled_imgs:
            label_path = output_dir / "labels" / f"{img.stem}.txt"
            label_path.write_text("")  # 空标注 = 负样本
            shutil.copy2(img, output_dir / "images" / img.name)
            count += 1
            neg_count += 1
    elif unlabeled_imgs:
        print(f"\n跳过 {len(unlabeled_imgs)} 张无标注图片 (负样本已禁用)")
        # 但不删除对应的空 label 文件

    if count == 0:
        print("未检测到有效数据, 请确认数据集路径正确")
        return

    # ---- 划分训练/验证集 ----
    if split and 0 < split < 1:
        split_train_val(output_dir, split)

    # ---- 生成 dataset.yaml ----
    idx_to_class = {v: k for k, v in class_map.items()}
    data_yaml = {
        "path": str(output_dir.resolve()),
        "nc": len(class_map),
        "names": idx_to_class,
    }

    if (output_dir / "train").exists():
        data_yaml["train"] = "train/images"
        data_yaml["val"] = "val/images"
    else:
        data_yaml["train"] = "images"
        data_yaml["val"] = "images"

    yaml_path = output_dir / "dataset.yaml"
    import yaml as _yaml
    with open(yaml_path, "w", encoding="utf-8") as f:
        _yaml.dump(data_yaml, f, allow_unicode=True, default_flow_style=False)

    print(f"\n========== 转换完成 ==========")
    print(f"图片总数: {count}")
    print(f"负样本数: {neg_count}")
    print(f"类别: {idx_to_class}")
    print(f"最小框过滤: {min_bbox}px {'(启用)' if min_bbox > 0 else '(禁用)'}")
    print(f"训练/验证集划分: {split if split else '(未划分)'}")
    print(f"配置文件: {yaml_path}")


def main():
    parser = argparse.ArgumentParser(description="MaixHub 数据集 → YOLO 格式")

    parser.add_argument("--input", "-i", required=True, help="数据集目录")
    parser.add_argument("--output", "-o", default="./dataset_yolo", help="输出目录")
    parser.add_argument("--labels", "-l", nargs="+", help="类别名称列表, 如: -l ball circle")

    # ---- 过滤 & 负样本 (对应 MaixHub 选项) ----
    filt = parser.add_argument_group("标注过滤 (对应 MaixHub '标注框限制' 和 '允许负样本')")
    filt.add_argument("--min-bbox", type=int, default=0,
                      help="最小标注框像素大小, 过滤更小的框 (默认: 0=不过滤, MaixHub 默认 10)")
    filt.add_argument("--keep-negatives", action="store_true", default=False,
                      help="保留无标注的图片作为负样本 (默认: 否)")

    # ---- 数据集划分 ----
    split = parser.add_argument_group("数据集划分")
    split.add_argument("--split", type=float, default=None,
                       help="训练集比例, 如 0.8 表示 80%% 训练 20%% 验证")

    args = parser.parse_args()

    class_map = {}
    if args.labels:
        class_map = {name: i for i, name in enumerate(args.labels)}

    keep_neg = args.keep_negatives

    auto_detect_and_convert(
        args.input, args.output, class_map,
        min_bbox=args.min_bbox,
        keep_negatives=keep_neg,
        split=args.split,
    )


if __name__ == "__main__":
    main()
