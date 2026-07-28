"""一键导出 4 个模型为 NCNN 格式（树莓派用）

用法: python convert_all_ncnn.py
"""

import shutil
from pathlib import Path
from ultralytics import YOLO

BASE = Path(__file__).parent
FINAL = BASE / "final_models"
NCNN_DIR = FINAL / "ncnn_models"
NCNN_DIR.mkdir(parents=True, exist_ok=True)

MODELS = [
    "yolo11n_ironball",
    "yolo11s_ironball",
    "yolo26n_ironball",
    "yolo26s_ironball",
]

for name in MODELS:
    pt = FINAL / f"{name}.pt"
    if not pt.exists():
        print(f"跳过 {name}: 文件不存在")
        continue

    print(f"导出 NCNN: {name} ...")
    model = YOLO(str(pt))
    result_dir = model.export(format="ncnn", imgsz=448, half=True)
    result_dir = Path(result_dir)

    dest = NCNN_DIR / name
    if dest.exists():
        shutil.rmtree(dest)
    shutil.move(str(result_dir), str(dest))
    print(f"  → {dest}/")

print(f"\n树莓派模型: {NCNN_DIR}")
for d in sorted(NCNN_DIR.glob("*")):
    if d.is_dir():
        print(f"  {d.name}/")
