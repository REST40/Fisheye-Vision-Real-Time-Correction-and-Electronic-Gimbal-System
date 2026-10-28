# PS 程序

`7ev_fish_app/src` 为当前板端应用源码，包含 UDP 协议、帧槽管理、宏观帧间流水调度、PL 驱动、映射表生成与校验、板端计时和遥测。

## 创建平台

使用 Vitis / XSCT 2022.2，在仓库根目录执行：

```bash
xsct PS/scripts/create_platform.tcl
```

默认工作区为 `PS/build`；环境变量 `FPGA_RACE_PS_WORKSPACE` 可指定另一个空工作区。平台读取 `PL/hardware_export/pl_project_snapshot.xsa`，处理器为 `psu_cortexa53_0`，64-bit standalone，添加 `xilffs 4.8` 和 `lwip211 1.8`，标准输入/输出使用 `psu_uart_0`。

## 创建应用

1. 在 Vitis 打开上述工作区，选择生成的平台及 standalone_domain，创建名为 `7ev_fish_app` 的 Empty Application。
2. 将本仓库 `PS/7ev_fish_app/src` 全部导入应用 src，使用随源码提供的 `lscript.ld`，避免重复的默认 main 或平台源文件。
3. 链接器添加数学库 `m`，确认 BSP 的 lwIP、xilffs、xil 库已关联。
4. 应用 IDE 构建配置见 `config/original_app.cproject`，其中 Debug 配置为 `-O0 -g3`。进行性能测试时，请记录实际使用的优化等级与构建配置。
5. 构建应用。需要启动镜像时，按 Zynq UltraScale+ 平台正常流程组合生成的 FSBL、对应 PL 配置和应用 ELF。

`config/system.mss` 提供 BSP 配置参考。BSP 驱动源码、库、头文件和 FSBL/PMU 产物由 Vitis 根据 PL XSA 生成；板端应用与启动镜像应使用匹配的硬件平台。

默认网络参数在 `7ev_fish_app/src/main.c` 等源文件中维护。调度入口与硬件驱动主要位于 `fptz_server.c`、`fptz_board.c`、`fptz_media_hw.c` 和 `fptz_corrector.c`。

