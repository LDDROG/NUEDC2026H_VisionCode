# 2026 全国大学生电子设计大赛 H 题 —— 视觉部分代码

本仓库为 2026 全国大学生电子设计大赛 H 题的视觉部分实现，包含无线图传与钢球识别两大模块。

## 系统概览

本视觉系统使用一台 **MaixCAM** 和一台 **MaixCAM Pro**，分别负责无线图传和钢球识别：

| 设备 | 职责 | 核心技术 |
| :---: | :--- | :--- |
| MaixCAM | 无线图传 | RTSP 推流 + FFmpeg 录制 |
| MaixCAM Pro | 钢球识别 | 自制 YOLO 模型实时推理 |

## 功能模块

### 无线图传

- 使用 **RTSP** 协议推流；
- 在 Windows 端使用 **FFplay** 进行实时直播；
- 同时利用 **FFmpeg** 进行录像存储。

### 钢球识别

- 使用由 **4844 张实拍图片**训练而成的自制 YOLO 模型；
- 预训练权重为 **YOLO26s**，迭代 **100** 轮；
- 最终 **mAP 达到 95.5%**；
- 在 MaixCAM Pro 上推理帧率约为 **50 FPS**。

## 仓库结构

```
NUEDC2026H_VisionCode/
├── 2026H/
│   ├── maixcam_rtsp_stream_improve/   # MaixCAM 无线图传（RTSP 推流）
│   ├── maixcampro_ironball_catching/  # MaixCAM Pro 钢球识别
│   └── pad_recording_package/         # 图传录像相关脚本
├── practice/                          # 历届赛题练习
│   ├── 2021F/
│   ├── 2023E/
│   ├── 2025E/
│   └── 2026Simulate/
├── yolo_training/                     # 数据集整理、模型训练与导出
├── yolo_maixcam_ironball_build/       # MaixCAM 端推理运行代码
├── TJC_T1Screen/                      # 淘晶驰串口屏换题基础工程
├── LICENSE.md
└── README.md
```

## 开源说明

仓库中附带使用**淘晶驰串口屏进行换题**的基础工程文件；本系统的主控部分暂不开源。

## 许可证

本项目基于 [MIT License](LICENSE.md) 开源。
