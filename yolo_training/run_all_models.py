"""串行训练 4 个 YOLO 模型，避免爆显存。睡前跑一行，起床拿结果。

用法: python run_all_models.py
"""

import shutil
import traceback
from pathlib import Path
from ultralytics import YOLO

# 跳过 AMP 检查 (Windows + PyTorch 已知卡死问题)
from ultralytics.engine.trainer import BaseTrainer
BaseTrainer.check_amp = lambda self, model: None

MODELS = [
    ("yolo11n.pt", "yolo11n_ironball"),
    ("yolo11s.pt", "yolo11s_ironball"),
    ("yolo26n.pt", "yolo26n_ironball"),
    ("yolo26s.pt", "yolo26s_ironball"),
]

DATA = "dataset_yolo/dataset.yaml"
EPOCHS = 100
IMGSZ = 448
BATCH = 16

OUTPUT_DIR = Path("final_models")
OUTPUT_DIR.mkdir(parents=True, exist_ok=True)

results_summary = []

for weight, name in MODELS:
    print(f"\n{'='*60}")
    print(f">>> 训练: {name} ({weight})")
    print(f"{'='*60}")

    try:
        model = YOLO(weight)
        model.train(
            data=DATA,
            epochs=EPOCHS,
            imgsz=IMGSZ,
            batch=BATCH,
            name=name,
            fliplr=0.5,
            degrees=10,
            workers=0,
            amp=False,
            plots=False,
            device="cuda",
        )

        # 找到训练输出目录
        runs = sorted(Path("runs/detect").glob(f"{name}*"), key=lambda p: p.stat().st_ctime)
        best_pt = runs[-1] / "weights" / "best.pt" if runs else None

        if best_pt and best_pt.exists():
            dst = OUTPUT_DIR / f"{name}.pt"
            shutil.copy2(best_pt, dst)
            print(f"  已保存: {dst}")

            # 用 best.pt 验证
            val_model = YOLO(str(dst))
            val_results = val_model.val(data=DATA, imgsz=IMGSZ, batch=BATCH, workers=0, device="cuda")
            mAP50 = val_results.box.map50
            results_summary.append((name, mAP50))
            print(f"  mAP50: {mAP50:.4f}")
        else:
            print(f"  警告: 找不到 best.pt")
            results_summary.append((name, None))

    except Exception as e:
        print(f"  错误: {e}")
        traceback.print_exc()
        results_summary.append((name, None))
        continue

print(f"\n{'='*60}")
print("全部完成!")
print(f"{'='*60}")
print(f"\n模型文件: {OUTPUT_DIR.resolve()}/")
for f in sorted(OUTPUT_DIR.glob("*.pt")):
    size_mb = f.stat().st_size / (1024 * 1024)
    print(f"  {f.name}  ({size_mb:.1f} MB)")

print(f"\n准确率汇总:")
for name, mAP in results_summary:
    score = f"mAP50={mAP:.4f}" if mAP else "训练失败"
    print(f"  {name}: {score}")
