#!/bin/bash
# YOLO Detection — Docker 训练启动脚本
# 用法:
#   bash docker-run.sh                              (默认: yolo26s 448x448)
#   bash docker-run.sh -w yolo26n.pt -s 320x64       (自定义参数)
#   bash docker-run.sh --balance                      (启用数据均衡)

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
IMAGE="yolo-train:latest"

echo "========================================"
echo "  YOLO Detection — Docker 训练"
echo "========================================"

# 构建镜像 (首次或 Dockerfile 变更时)
if [[ -z "$(docker images -q $IMAGE 2>/dev/null)" ]]; then
    echo "[1/2] 构建 Docker 镜像..."
    docker build -t $IMAGE "$SCRIPT_DIR"
else
    echo "[1/2] 镜像已存在，跳过构建"
fi

# 默认参数: yolo26s, 448x448, 100 epochs, batch 32
ARGS="${@:--w yolo26s.pt -s 448 -e 100 -b 32 --name yolo26s_448}"

echo "[2/2] 启动训练容器..."
echo "  参数: $ARGS"
echo

docker run --gpus all -it --rm \
    -v "$SCRIPT_DIR":/workspace \
    $IMAGE \
    train.py --data ./dataset_yolo/dataset.yaml $ARGS

echo
echo "训练结束。模型保存在: $SCRIPT_DIR/runs/detect/"
