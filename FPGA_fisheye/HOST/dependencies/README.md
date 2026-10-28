# 外部相机 SDK

从 [Insta360 官方开发者平台](https://www.insta360.com/developer/home) 申请/获取 Linux SDK。本项目使用 CameraSDK 2.1.1 与 MediaSDK 3.1.1；使用其他版本时需确认 API 和运行库兼容性。

本仓库不附带 SDK 头文件、共享库或模型。将所需内容放置为：

```text
HOST/dependencies/
  CameraSDK-2.1.1/
    include/
    lib/libCameraSDK.so
    lib/其他随 SDK 提供的运行库
  MediaSDK-3.1.1/
    include/
    lib/libMediaSDK.so
    lib/其他随 SDK 提供的运行库
    models/
```

保持完整的依赖库和模型目录结构；程序依据 `HOST/config/sdk.json` 使用这些路径。不要只复制两个主 .so 而遗漏它们的依赖。
无 SDK 时使用 CMake 选项 `-DBUILD_X5_CAPTURE=OFF` 构建软件主体与测试。
