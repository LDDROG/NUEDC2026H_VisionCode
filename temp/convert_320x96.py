"""转换 yolo11s + yolo26s → 320×96 cvimodel + MUD"""
import json
import shutil
import subprocess
import zipfile
from pathlib import Path

BASE = Path(__file__).parent.parent  # NUEDCVisionAndPTCode/
SRC = BASE / "待转换"
TEMP = BASE / "temp"
FINAL = BASE / "2026H" / "maixcampro_ironball_catching"
CALIB_SRC = BASE / "yolo_training" / "dataset_yolo" / "val" / "images"
DOCKER_IMG = "sophgo/tpuc_dev:latest"

# 分辨率: W=320, H=96  (NCHW: [1,3,96,320])
IMGW, IMGH = 320, 96
MEAN = "0,0,0"
SCALE = "0.0039216,0.0039216,0.0039216"
LABELS = ["ironball"]

MODELS = [
    {
        "name": "yolo11s_ironball_to320x96",
        "pt": "yolo11s_ironball_to320x96.pt",
        "outputs": "/model.23/dfl/conv/Conv_output_0,/model.23/Sigmoid_output_0",
        "model_type": "yolo11",
    },
    {
        "name": "yolo26s_ironball_to320x96",
        "pt": "yolo26s_ironball_to320x96.pt",
        "outputs": (
            "/model.23/one2one_cv2.0/one2one_cv2.0.2/Conv_output_0,"
            "/model.23/one2one_cv2.1/one2one_cv2.1.2/Conv_output_0,"
            "/model.23/one2one_cv2.2/one2one_cv2.2.2/Conv_output_0,"
            "/model.23/one2one_cv3.0/one2one_cv3.0.2/Conv_output_0,"
            "/model.23/one2one_cv3.1/one2one_cv3.1.2/Conv_output_0,"
            "/model.23/one2one_cv3.2/one2one_cv3.2.2/Conv_output_0"
        ),
        "model_type": "yolo26",
    },
]


def export_onnx(pt_path, output_dir):
    """PT → ONNX at 320×96"""
    from ultralytics import YOLO
    model = YOLO(str(pt_path))
    onnx_path = model.export(
        format="onnx",
        imgsz=(IMGH, IMGW),  # ultralytics: (height, width)
        opset=12,
        simplify=True,
        half=False,
        dynamic=False,
        batch=1,
    )
    onnx_file = Path(onnx_path)
    dest = output_dir / f"{pt_path.stem}.onnx"
    if onnx_file != dest:
        shutil.move(str(onnx_file), str(dest))
    print(f"  ONNX → {dest}")
    return dest


def trim_onnx(onnx_path, output_names, output_dir):
    """裁剪 ONNX 输出节点（去除后处理，只留检测头输出）"""
    import onnx
    out_names = [n.strip() for n in output_names.split(",")]
    model = onnx.load(str(onnx_path))
    # 提取子图
    e = onnx.utils.Extractor(model)
    # 检查哪些输出存在
    all_nodes = {n.name for n in model.graph.node}
    all_outputs = {o.name for o in model.graph.output}
    valid = [n for n in out_names if n in all_nodes or n in all_outputs]
    if not valid:
        print(f"  ⚠ 输出节点不在图中，跳过裁剪: {out_names}")
        return onnx_path
    extracted = e.extract_model(
        input_names=[i.name for i in model.graph.input],
        output_names=valid,
    )
    out_path = output_dir / f"{onnx_path.stem}_trimmed.onnx"
    onnx.save(extracted, str(out_path))
    # onnxsim 精简
    import onnxsim
    sim_model, ok = onnxsim.simplify(str(out_path))
    if ok:
        onnx.save(sim_model, str(out_path))
    print(f"  裁剪 → {out_path}  (outputs: {valid})")
    return out_path


def prepare_calib(output_dir):
    """准备校准图片"""
    calib_dest = output_dir / "calib_images"
    if calib_dest.exists():
        shutil.rmtree(calib_dest)
    calib_dest.mkdir(parents=True)
    imgs = sorted(CALIB_SRC.glob("*"))[:100]
    for img in imgs:
        shutil.copy2(img, calib_dest / img.name)
    print(f"校准集: {len(imgs)} 张 → {calib_dest}")
    return calib_dest


def docker_cmd(cmd_str):
    """在 Docker 容器中运行"""
    cmd = [
        "docker", "run", "--rm",
        "-v", f"{TEMP.resolve().as_posix()}:/workspace",
        "-w", "/workspace",
        DOCKER_IMG,
        "bash", "-c", f"pip install tpu_mlir -q && {cmd_str}",
    ]
    print(f"  [Docker] {cmd_str[:120]}...")
    r = subprocess.run(cmd, check=False, capture_output=True, text=True)
    if r.returncode != 0:
        print(f"  STDERR: {r.stderr[-500:] if r.stderr else 'none'}")
    else:
        # 打印最后几行
        lines = r.stdout.strip().split("\n")
        for l in lines[-3:]:
            print(f"    {l}")
    return r.returncode


def make_mud(cvimodel_path, model_type, output_dir):
    """创建 INI 格式 MUD"""
    mud_path = output_dir / f"{cvimodel_path.stem}.mud"
    content = f"""[basic]
type = cvimodel
model = {cvimodel_path.name}

[extra]
model_type = {model_type}
input_type = rgb
mean = {MEAN}
scale = {SCALE}
labels = {",".join(LABELS)}
"""
    mud_path.write_text(content, encoding="utf-8")
    print(f"  MUD → {mud_path}")
    return mud_path


def main():
    print("=" * 60)
    print(f"转换模型 → 320×96 cvimodel + MUD")
    print("=" * 60)

    # 0. 准备校准图片
    calib_dir = prepare_calib(TEMP)

    for model in MODELS:
        name = model["name"]
        pt_file = SRC / model["pt"]
        if not pt_file.exists():
            print(f"\n跳过 {name}: PT 文件不存在 {pt_file}")
            continue

        print(f"\n{'='*60}")
        print(f"转换: {name}")
        print(f"{'='*60}")

        # 1. PT → ONNX
        print("  [1/5] PT → ONNX...")
        onnx_path = export_onnx(pt_file, TEMP)

        # 2. 裁剪 ONNX
        print("  [2/5] 裁剪 ONNX...")
        trimmed_onnx = trim_onnx(onnx_path, model["outputs"], TEMP)

        # 准备路径 (Docker 视角, /workspace=temp)
        mlir_file = f"/workspace/{name}.mlir"
        onnx_docker = f"/workspace/{trimmed_onnx.name}"
        cali_table = f"/workspace/{name}_cali"
        cvimodel_docker = f"/workspace/{name}.cvimodel"

        # 3. ONNX → MLIR
        print("  [3/5] ONNX → MLIR (Docker)...")
        mlir_cmd = (
            f"model_transform.py "
            f"--model_name {name} "
            f"--model_def {onnx_docker} "
            f"--input_shapes [[1,3,{IMGH},{IMGW}]] "
            f"--mean {MEAN} "
            f"--scale {SCALE} "
            f"--pixel_format rgb "
            f"--channel_format nchw "
            f"--output_names '{model['outputs']}' "
            f"--mlir {mlir_file}"
        )
        rc = docker_cmd(mlir_cmd)
        if rc != 0:
            print(f"  ❌ MLIR 转换失败")
            continue

        # 4. 校准
        print("  [4/5] 生成校准表 (Docker)...")
        calib_cmd = (
            f"run_calibration.py "
            f"{mlir_file} "
            f"--dataset /workspace/calib_images "
            f"--input_num 100 "
            f"-o {cali_table}"
        )
        rc = docker_cmd(calib_cmd)
        if rc != 0:
            print(f"  ❌ 校准失败")
            continue

        # 5. MLIR → cvimodel
        print("  [5/5] MLIR → cvimodel (Docker)...")
        deploy_cmd = (
            f"model_deploy.py "
            f"--mlir {mlir_file} "
            f"--quantize INT8 "
            f"--calibration_table {cali_table} "
            f"--processor cv181x "
            f"--model {cvimodel_docker}"
        )
        rc = docker_cmd(deploy_cmd)
        if rc != 0:
            print(f"  ❌ cvimodel 生成失败")
            continue

        # 6. 创建 MUD + 复制到 final 目录
        cvimodel_local = TEMP / f"{name}.cvimodel"
        if cvimodel_local.exists():
            mud = make_mud(cvimodel_local, model["model_type"], TEMP)
            # 复制到 maixcampro 目录
            for f in [cvimodel_local, mud]:
                dest = FINAL / f.name
                shutil.copy2(str(f), str(dest))
            print(f"  ✅ 已复制到 maixcampro_ironball_catching/")
        else:
            print(f"  ❌ cvimodel 未生成: {cvimodel_local}")

    # 汇总
    print(f"\n{'='*60}")
    print("转换完成! 输出文件:")
    for f in sorted(TEMP.glob("*")):
        if f.suffix in (".cvimodel", ".mud"):
            sz = f.stat().st_size
            print(f"  {f.name}  ({sz/1024:.0f} KB)")
    print(f"{'='*60}")


if __name__ == "__main__":
    main()
