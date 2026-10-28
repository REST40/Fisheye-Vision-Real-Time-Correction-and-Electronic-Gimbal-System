# PL 工程

器件为 `xczu7ev-ffvc1156-2-i`，开发工具为 Vivado 2022.2。系统包含前向、后向两个 JPEG 解码器、鱼眼校正融合 IP、JPEG 编码器、AXI DMA、GPIO 和 AXI 互连，全景输出尺寸为 2656×1328。

## 创建与构建工程

在仓库根目录执行：

```bash
vivado -mode batch -source PL/scripts/create_project.tcl
```

也可在未打开工程的 Vivado Tcl Console 中 source 此脚本。默认工程路径为 `PL/build/fisheye_mjpeg.xpr`，可通过环境变量 `FPGA_RACE_PL_BUILD` 指定新的生成目录。脚本根据 Block Design、源文件列表和本地 IP 仓库创建工程及顶层包装器。

打开工程后依次执行 Run Synthesis、Run Implementation、Generate Bitstream，再按需要执行 Export Hardware。更新 XSA 后，应同步重新生成 PS 平台。

## 源码目录

- `mjpeg/rtl`：AXI 包装、解码写回、编码输入和存储辅助模块。
- `mjpeg/adapted`：适配项目尺寸、数据格式和接口的编解码模块。
- `fisheye_ip/ip_repo/fisheye_cort_1_0/src`：校正、插值、源行缓存与融合电路。
- `design/mjpeg_system.bd`：系统连接和参数配置。
- `mjpeg/third_party`：第三方源码与许可证；固定版本见 `mjpeg/upstream_lock.json`。

## 硬件平台文件

`hardware_export/pl_project_snapshot.xsa` 用于创建 PS 开发平台。重新导出硬件平台时，应使用与 PL 配置匹配的 XSA，并同步更新板端应用所依赖的平台。
