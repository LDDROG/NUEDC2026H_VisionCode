"""一键转换 4 个模型 → cvimodel + MUD（使用 Docker tpu-mlir）

用法: python convert_all_docker.py
前置: Docker Desktop 已安装并运行
"""

import json
import shutil
import subprocess
import zipfile
from pathlib import Path

BASE = Path(__file__).parent
FINAL = BASE / "final_models"
CALIB_IMG = BASE / "dataset_yolo/val/images"
DOCKER_IMG = "sophgo/tpuc_dev:latest"

# 四个模型配置
MODELS = [
    {
        "name": "yolo11n_ironball",
        "onnx": "yolo11n_ironball_cropped.onnx",
        "outputs": "/model.23/dfl/conv/Conv_output_0,/model.23/Sigmoid_output_0",
        "model_type": "yolo11",
    },
    {
        "name": "yolo11s_ironball",
        "onnx": "yolo11s_ironball_cropped.onnx",
        "outputs": "/model.23/dfl/conv/Conv_output_0,/model.23/Sigmoid_output_0",
        "model_type": "yolo11",
    },
    {
        "name": "yolo26n_ironball",
        "onnx": "yolo26n_ironball_cropped.onnx",
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
    {
        "name": "yolo26s_ironball",
        "onnx": "yolo26s_ironball_cropped.onnx",
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

IMGSZ = 448
MEAN = "0,0,0"
SCALE = "0.0039216,0.0039216,0.0039216"
LABELS = ["ironball"]


def make_mud(cvimodel_path, model_type, output_dir):
    """创建 INI 格式 MUD 文件"""
    mud_path = output_dir / cvimodel_path.with_suffix(".mud").name
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
    return mud_path


def docker_cmd(args_list):
    """在 Docker 容器中运行命令"""
    cmd = [
        "docker", "run", "--rm",
        "-v", f"{BASE.resolve().as_posix()}:/workspace",
        "-w", "/workspace/final_models",
        DOCKER_IMG,
        "bash", "-c", "pip install tpu_mlir -q && " + args_list,
    ]
    return subprocess.run(cmd, check=False)


def main():
    # 校准集复制到 final_models
    calib_dest = FINAL / "calib_images"
    if not calib_dest.exists():
        calib_dest.mkdir(parents=True)
        imgs = sorted(CALIB_IMG.glob("*"))[:100]
        for img in imgs:
            shutil.copy2(img, calib_dest / img.name)
        print(f"校准集: {len(imgs)} 张 → {calib_dest}")

    for model in MODELS:
        name = model["name"]
        onnx_file = FINAL / model["onnx"]
        if not onnx_file.exists():
            print(f"跳过 {name}: {model['onnx']} 不存在")
            continue

        print(f"\n{'='*60}")
        print(f"转换: {name}")
        print(f"{'='*60}")

        mlir_base = f"{name}"

        # 1. ONNX → MLIR
        mlir_cmd = (
            f"model_transform.py "
            f"--model_name {name} "
            f"--model_def /workspace/final_models/{model['onnx']} "
            f"--input_shapes [[1,3,{IMGSZ},{IMGSZ}]] "
            f"--mean {MEAN} "
            f"--scale {SCALE} "
            f"--pixel_format rgb "
            f"--channel_format nchw "
            f"--output_names '{model['outputs']}' "
            f"--mlir /workspace/final_models/{mlir_base}.mlir"
        )
        print(f"  [1/3] ONNX → MLIR...")
        r = docker_cmd(mlir_cmd)
        if r.returncode != 0:
            print(f"  ❌ 失败")
            continue

        # 2. 生成校准表
        calib_cmd = (
            f"run_calibration.py "
            f"/workspace/final_models/{mlir_base}.mlir "
            f"--dataset /workspace/final_models/calib_images "
            f"--input_num 100 "
            f"-o /workspace/final_models/{mlir_base}_cali"
        )
        print(f"  [2/3] 生成校准表...")
        r = docker_cmd(calib_cmd)
        if r.returncode != 0:
            print(f"  ❌ 失败")
            continue

        # 3. MLIR → cvimodel
        deploy_cmd = (
            f"model_deploy.py "
            f"--mlir /workspace/final_models/{mlir_base}.mlir "
            f"--quantize INT8 "
            f"--calibration_table /workspace/final_models/{mlir_base}_cali "
            f"--processor cv181x "
            f"--model /workspace/final_models/{mlir_base}.cvimodel"
        )
        print(f"  [3/3] MLIR → cvimodel...")
        r = docker_cmd(deploy_cmd)
        if r.returncode != 0:
            print(f"  ❌ 失败")
            continue

        # 4. 创建 MUD
        cvimodel = FINAL / f"{mlir_base}.cvimodel"
        if cvimodel.exists():
            mud = make_mud(cvimodel, model["model_type"], FINAL)
            print(f"  ✅ {cvimodel.name} + {mud.name}")
        else:
            print(f"  ❌ cvimodel 未生成")

    # 汇总
    print(f"\n{'='*60}")
    print("转换完成! MaixCam 可用文件:")
    for f in sorted(FINAL.glob("*.cvimodel")):
        mud = FINAL / f"{f.stem}.mud"
        status = "✓" if mud.exists() else "✗"
        print(f"  {status} {f.name}  +  {mud.name}")
    print(f"{'='*60}")


if __name__ == "__main__":
    main()
