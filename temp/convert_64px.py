"""yolo26s → 256×64 + 320×64 cvimodel + MUD"""
import shutil, subprocess
from pathlib import Path

BASE = Path(__file__).parent.parent
SRC = BASE / "待转换"
TEMP = BASE / "temp"
FINAL = BASE / "2026H" / "maixcampro_ironball_catching"
CALIB_SRC = BASE / "yolo_training" / "dataset_yolo" / "val" / "images"
DOCKER_IMG = "sophgo/tpuc_dev:latest"

PT = SRC / "yolo26s_ironball_to320x96.pt"
OUTPUTS = (
    "/model.23/one2one_cv2.0/one2one_cv2.0.2/Conv_output_0,"
    "/model.23/one2one_cv2.1/one2one_cv2.1.2/Conv_output_0,"
    "/model.23/one2one_cv2.2/one2one_cv2.2.2/Conv_output_0,"
    "/model.23/one2one_cv3.0/one2one_cv3.0.2/Conv_output_0,"
    "/model.23/one2one_cv3.1/one2one_cv3.1.2/Conv_output_0,"
    "/model.23/one2one_cv3.2/one2one_cv3.2.2/Conv_output_0"
)
MODEL_TYPE = "yolo26"

RESOLUTIONS = [
    {"name": "yolo26s_ironball_to256x64", "w": 256, "h": 64},
    {"name": "yolo26s_ironball_to320x64", "w": 320, "h": 64},
]

MEAN = "0,0,0"
SCALE = "0.0039216,0.0039216,0.0039216"
LABELS = ["ironball"]


def docker_cmd(cmd_str):
    r = subprocess.run([
        "docker", "run", "--rm",
        "-v", f"{TEMP.resolve().as_posix()}:/workspace",
        "-w", "/workspace",
        DOCKER_IMG,
        "bash", "-c", f"pip install tpu_mlir -q && {cmd_str}",
    ], check=False, capture_output=True, text=True)
    if r.returncode != 0:
        print(f"  ❌ {r.stderr[-300:] if r.stderr else 'none'}")
    return r.returncode


def make_mud(cv_path, model_type):
    mud_path = cv_path.with_suffix(".mud")
    mud_path.write_text(f"""[basic]
type = cvimodel
model = {cv_path.name}

[extra]
model_type = {model_type}
input_type = rgb
mean = {MEAN}
scale = {SCALE}
labels = {",".join(LABELS)}
""", encoding="utf-8")
    return mud_path


def convert_one(name, w, h):
    print(f"\n{'='*60}")
    print(f"转换: {name}  ({w}×{h})")
    print(f"{'='*60}")

    # 1. PT → ONNX
    print("  [1/4] PT → ONNX...")
    from ultralytics import YOLO
    model = YOLO(str(PT))
    onnx_tmp = model.export(format="onnx", imgsz=(h, w), opset=12,
                            simplify=True, half=False, dynamic=False, batch=1)
    onnx_dest = TEMP / f"{name}.onnx"
    shutil.move(str(onnx_tmp), str(onnx_dest))
    print(f"    → {onnx_dest.name}")

    # 2. ONNX → MLIR (Docker)
    print("  [2/4] ONNX → MLIR...")
    rc = docker_cmd(
        f"model_transform.py "
        f"--model_name {name} "
        f"--model_def /workspace/{name}.onnx "
        f"--input_shapes [[1,3,{h},{w}]] "
        f"--mean {MEAN} --scale {SCALE} "
        f"--pixel_format rgb --channel_format nchw "
        f"--output_names '{OUTPUTS}' "
        f"--mlir /workspace/{name}.mlir"
    )
    if rc: return

    # 3. 校准 + 部署 (Docker)
    print("  [3/4] 校准+部署...")
    rc = docker_cmd(
        f"run_calibration.py /workspace/{name}.mlir "
        f"--dataset /workspace/calib_images --input_num 100 "
        f"-o /workspace/{name}_cali && "
        f"model_deploy.py "
        f"--mlir /workspace/{name}.mlir --quantize INT8 "
        f"--calibration_table /workspace/{name}_cali "
        f"--processor cv181x "
        f"--model /workspace/{name}.cvimodel"
    )
    if rc: return

    # 4. MUD
    cv_local = TEMP / f"{name}.cvimodel"
    if cv_local.exists():
        mud = make_mud(cv_local, MODEL_TYPE)
        for f in [cv_local, mud]:
            shutil.copy2(str(f), str(FINAL / f.name))
        print(f"  [4/4] ✅ {cv_local.name} + {mud.name}")
    else:
        print(f"  ❌ cvimodel 未生成")


# 校准图片（复用上一轮 temp 里已有的）
calib_dest = TEMP / "calib_images"
if not calib_dest.exists():
    calib_dest.mkdir(parents=True)
    imgs = sorted(CALIB_SRC.glob("*"))[:100]
    for img in imgs:
        shutil.copy2(img, calib_dest / img.name)
    print(f"校准集: {len(imgs)} 张")

for r in RESOLUTIONS:
    convert_one(r["name"], r["w"], r["h"])

print(f"\n{'='*60}")
print("转换完成!")
for f in sorted(TEMP.glob("yolo26s_ironball_to*x*.cvimodel")):
    print(f"  {f.name}  ({f.stat().st_size/1024:.0f} KB)")
