"""将 labels 3 和 labels 4 中的 YOLO txt 转为 VOC XML，写入各自 xml/ 目录。"""
import xml.etree.ElementTree as ET
from xml.dom import minidom
from pathlib import Path
from PIL import Image

LABEL_MAP = {0: "ironball"}
BASE = Path(r"f:\Code\NUEDCVisionAndPTCode\yolov11n_training\钢球数据集")

for folder_name in ["labels 3", "labels 4"]:
    label_dir = BASE / folder_name / "labels"
    img_dir = BASE / folder_name / "images"
    xml_dir = BASE / folder_name / "xml"
    xml_dir.mkdir(parents=True, exist_ok=True)

    txt_files = sorted(label_dir.glob("*.txt"))
    print(f"\n{folder_name}: {len(txt_files)} 个 txt 文件")

    for txt_path in txt_files:
        # 找对应图片
        img_path = None
        for ext in (".jpg", ".jpeg", ".png", ".bmp"):
            candidate = img_dir / f"{txt_path.stem}{ext}"
            if candidate.exists():
                img_path = candidate
                break
        if img_path is None:
            print(f"  跳过 {txt_path.name}: 找不到对应图片")
            continue

        # 读图片尺寸
        with Image.open(img_path) as im:
            img_w, img_h = im.size

        # 读 YOLO 标注
        content = txt_path.read_text(encoding="utf-8").strip()

        # 构建 VOC XML
        annotation = ET.Element("annotation")

        folder = ET.SubElement(annotation, "folder")
        folder.text = folder_name

        filename = ET.SubElement(annotation, "filename")
        filename.text = img_path.name

        size = ET.SubElement(annotation, "size")
        ET.SubElement(size, "width").text = str(img_w)
        ET.SubElement(size, "height").text = str(img_h)
        ET.SubElement(size, "depth").text = "3"

        if content:
            for line in content.splitlines():
                parts = line.strip().split()
                if len(parts) != 5:
                    continue
                cls_id = int(parts[0])
                cx, cy, w, h = map(float, parts[1:])

                # 归一化 → 像素坐标 (xyxy)
                x1 = int((cx - w / 2) * img_w)
                y1 = int((cy - h / 2) * img_h)
                x2 = int((cx + w / 2) * img_w)
                y2 = int((cy + h / 2) * img_h)

                # clamp
                x1 = max(0, min(x1, img_w - 1))
                y1 = max(0, min(y1, img_h - 1))
                x2 = max(1, min(x2, img_w))
                y2 = max(1, min(y2, img_h))

                obj = ET.SubElement(annotation, "object")
                obj_name = LABEL_MAP.get(cls_id, f"class_{cls_id}")
                ET.SubElement(obj, "name").text = obj_name
                ET.SubElement(obj, "difficult").text = "0"

                bndbox = ET.SubElement(obj, "bndbox")
                ET.SubElement(bndbox, "xmin").text = str(x1)
                ET.SubElement(bndbox, "ymin").text = str(y1)
                ET.SubElement(bndbox, "xmax").text = str(x2)
                ET.SubElement(bndbox, "ymax").text = str(y2)

        # 格式化输出
        xml_str = ET.tostring(annotation, encoding="unicode")
        pretty = minidom.parseString(xml_str).toprettyxml(indent="  ", encoding="utf-8")
        xml_path = xml_dir / f"{txt_path.stem}.xml"
        xml_path.write_bytes(pretty)

    print(f"  转换完成 → {xml_dir}")

print("\n全部完成!")
