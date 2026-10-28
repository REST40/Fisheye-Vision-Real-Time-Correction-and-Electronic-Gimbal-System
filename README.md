# Fisheye-Vision-Real-Time-Correction-and-Electronic-Gimbal-System
鱼眼视觉实时校正与电子云台系统
# 基于 FPGA 的双鱼眼全景图像处理系统

本仓库为 FPGA 创新设计竞赛项目源码，包含 PL 图像处理电路、PS 调度与网络程序，以及 C++ / Qt 上位机。

输入为 Insta360 X5 的 2656×1328 双鱼眼图像。上位机将同一帧拆分为两幅 1328×1328 图像并编码为 JPEG，经网络发送至 PS。PL 完成双路 JPEG 解码、鱼眼校正与融合、全景 JPEG 编码；PS 回传结果，上位机完成球体显示、虚拟云台浏览及性能分析。

## 仓库结构

```text
PL/
  design/mjpeg_system.bd             当前系统 Block Design
  mjpeg/rtl/                         编解码接口与辅助电路
  mjpeg/adapted/                     适配后的开源编解码模块
  mjpeg/third_party/                 原始开源源码与许可证
  fisheye_ip/ip_repo/                实际使用的鱼眼校正 IP
  scripts/create_project.tcl         Vivado 工程创建入口
  hardware_export/                   唯一保留的 PL XSA
PS/
  7ev_fish_app/src/                  板端应用源码
  config/                           BSP 配置及原应用编译配置
  scripts/create_platform.tcl        Vitis 平台创建入口
HOST/
  src/                              Qt 上位机源码
  tests/                            已有软件测试
  config/                           标定与 SDK 配置
  dependencies/README.md             外部 SDK 安装位置说明
  tools/                            构建、校验与演示数据生成工具
docs/                               系统说明与提交说明
tools/verify_repository.py           仓库完整性检查
```

## 开发环境与入口

| 部分 | 环境 | 入口 |
|---|---|---|
| PL | Vivado 2022.2；xczu7ev-ffvc1156-2-i | [PL/README.md](PL/README.md) |
| PS | Vitis / XSCT 2022.2；Cortex-A53 64-bit standalone；lwIP | [PS/README.md](PS/README.md) |
| HOST | Linux；C++17；CMake；Qt 6.4+；OpenCV；FFmpeg | [HOST/README.md](HOST/README.md) |

正式源码可直接浏览；工具生成的工程、BSP、目标文件和录制视频在本地生成。
XSA 仅保留 PL 当前工程目录内最新的导出，位置为 `PL/hardware_export/pl_project_snapshot.xsa`。
PS 使用这份 XSA 重新生成平台，仓库不收录 PS 历史 XSA。

## 快速检查

```bash
python3 tools/verify_repository.py
python3 HOST/tools/quality/verify_reference.py
```

第一个命令只检查源码包；第二个命令需要 GCC，会比较 Host 软件参考与 PS 映射表生成结果。
上位机可在关闭 X5 SDK 构建的模式下编译和执行软件测试，具体步骤见 HOST/README.md。

## 系统连接

默认上位机地址 `192.168.10.100`，板端地址 `192.168.10.10`。
UDP 5500 为控制与遥测，5501 为双 JPEG 输入，5502 为全景 JPEG 回传。
通信采用项目自定义 FPTZ v3 协议，完整全景模式为 2656×1328。
详见 [系统结构与数据流](docs/architecture.md)。
