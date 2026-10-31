# 板端程序（PS）

PS 运行于 Cortex-A53 64-bit standalone 环境，负责网络协议、帧槽管理、处理调度、PL 驱动、反向映射表生成和板端遥测。应用源码位于 [7ev_fish_app/src](7ev_fish_app/src)。

## 创建平台

使用 Vitis / XSCT 2022.2。先按 [PL 说明](../PL/README.md) 构建硬件，并导出 `PL/hardware_export/pl_project_snapshot.xsa`；随后在仓库根目录执行：

```bash
xsct PS/scripts/create_platform.tcl
```

默认工作区为 `PS/build`。环境变量 `FPGA_RACE_PS_WORKSPACE` 可指定新的工作区；已有平台可在 Vitis 中打开或更新。

| 配置项 | 设置 |
|---|---|
| 处理器 | `psu_cortexa53_0` |
| 系统 | 64-bit standalone |
| 库 | `xilffs 4.8`、`lwip211 1.8` |
| 标准输入 / 输出 | `psu_uart_0` |

完整 BSP 配置参考 [config/system.mss](config/system.mss)，应用编译配置参考 [config/original_app.cproject](config/original_app.cproject)。BSP 驱动、头文件、库和启动程序由 Vitis 根据 XSA 生成。

## 创建与构建应用

1. 在 Vitis 打开工作区，基于生成的平台和 `standalone_domain` 创建名为 `7ev_fish_app` 的 Empty Application。
2. 导入 `7ev_fish_app/src` 的全部源码，使用其中的 `lscript.ld`，移除模板中重复的入口和平台文件。
3. 链接数学库 `m`，确认 BSP 的 lwIP、xilffs 和 xil 库已关联。
4. BSP 和应用均添加 `-DLWIP_ALLOW_MEM_FREE_FROM_OTHER_CONTEXT=1`，并保持 `SYS_LIGHTWEIGHT_PROT=1`。平台脚本已设置 BSP 编译选项，应用侧仍需设置。修改 BSP 配置后，重新生成库并链接应用。
5. 构建应用。参考配置的 Debug 选项为 `-O0 -g3`；调整优化等级后，应重新检查运行行为并记录测试时使用的配置。

GEM 发送完成中断会回收 `PBUF_RAM`，因此主循环分配与中断释放需要上述 lwIP 堆保护设置。应用编译时会检查该配置，启动串口输出包含 `FPTZ_LWIP_HEAP irq_free_safe=1`。

## 启动与连接

通过 Vitis/JTAG 下载与当前硬件匹配的位流和应用 ELF，或使用同一平台生成的 FSBL、位流及应用组合启动镜像。启动文件为本地构建产物。

默认板端地址为 `192.168.10.10`，电脑地址为 `192.168.10.100`。网络参数在 `7ev_fish_app/src/main.c` 等源文件中维护；UDP 端口及上位机连接方式见 [仓库说明](../README.md)。

## 主要模块

| 源码 | 职责 |
|---|---|
| `fptz_server.c`、`fptz_scheduler.h` | 协议处理、帧槽和流水调度 |
| `fptz_board.c`、`fptz_media_hw.c` | 板级初始化、编解码与 DMA 驱动 |
| `fptz_corrector.c` | 校正 IP 控制与状态读取 |
| `fptz_lut_generate.c`、`fptz_geometry.h` | 几何模型、定点坐标和融合权重 |
| `fptz_protocol.c` | 协议字段与校验 |

## 软件检查

在仓库根目录执行以下命令，对照 HOST 和 PS 的完整映射表。需要 GCC，不需要连接板卡：

```bash
python3 HOST/tools/quality/verify_reference.py
```

其他原生 CPU 测试位于 `HOST/tests/ps`，运行脚本位于 `HOST/tools/ps`。
