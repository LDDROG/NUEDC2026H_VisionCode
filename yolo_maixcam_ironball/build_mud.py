"""打包 MUD: 把 cvimodel 放进此目录后运行 python build_mud.py"""
import json
import zipfile
from pathlib import Path

PROJECT_DIR = Path(__file__).parent
MODEL_NAME = "yolo11s_ironball"

# 查找 cvimodel
cvimodels = list(PROJECT_DIR.glob("*.cvimodel"))
if not cvimodels:
    print("错误: 未找到 .cvimodel 文件!")
    print("请先把转换好的 cvimodel 放到此目录, 再运行 python build_mud.py")
    exit(1)

cvimodel_path = cvimodels[0]
print(f"使用: {cvimodel_path.name}")

# 构建 report.json (与 MaixHub 格式对齐)
report = {
    "anchors": [10, 13, 16, 30, 33, 23, 30, 61, 62, 45,
                59, 119, 116, 90, 156, 198, 373, 326],
    "inputs": {"input0": [448, 448, 3]},
    "outputs": {
        "output0": [56, 56, 18],
        "output1": [28, 28, 18],
        "output2": [14, 14, 18],
    },
    "params_quantity": 9413187,
    "mean": 0,
    "std": 255,
    "label_type": "detection",
    "data_type": "image",
    "labels": ["ironball"],
    "best_eval": {"epoch": 0, "val_acc": 0.935, "val_info": []},
}

# 打包 MUD (ZIP)
mud_path = PROJECT_DIR / f"{MODEL_NAME}.mud"
with zipfile.ZipFile(str(mud_path), "w", zipfile.ZIP_DEFLATED) as zf:
    zf.write(cvimodel_path, cvimodel_path.name)
    zf.writestr("report.json", json.dumps(report, indent=2, ensure_ascii=False))

print(f"生成完成: {mud_path}")
print(f"\n项目结构:")
for f in sorted(PROJECT_DIR.rglob("*")):
    if f.is_file():
        size = f.stat().st_size
        print(f"  {f.relative_to(PROJECT_DIR)}  ({size/1024:.0f} KB)")
