# MagicPad Native PTP Driver

将 Apple Magic Trackpad 2 / USB‑C Magic Trackpad 的原始多点触控报告转换为
Windows 11 原生 Precision Touchpad（PTP）报告。Windows 接收的是标准 PTP 触点，
滚动、缩放、三指/四指手势仍由系统手势栈处理。

> 当前状态：开发预览。macOS 模拟器和共享转换核心可直接运行；KMDF 驱动需要在
> Windows 11 x64 + WDK 环境中构建，并在真实 USB‑C Magic Trackpad 上完成硬件门禁
> 后才能日常使用。仓库不包含可公开分发的 Microsoft 签名。

## 已实现

- Bluetooth 优先：已声明 Apple Bluetooth VID `0x004c`，PID `0x0265` / `0x0324`；
  `0x0324` 目前处于实机 bring-up
- USB‑C Magic Trackpad PID `0x0324` 的现有 USB 路径
- 原生 PTP HID 描述符与 5 触点、50 字节 Input Report
- Apple `0x31` Report 的显式小端解析，不依赖 C 位域布局
- 稳定的 5 触点准入、Palm Confidence、Near Finger、按压锁定与 Scan Time
- 驱动和 macOS 模拟器共用同一份无浮点、无动态分配的 C17 转换核心
- 本地 Web 模拟器：手动拖拽、手势预设、时间线和 trace 导入/导出
- WPP/ETW 元数据诊断；默认不记录原始触点字节

## 在 macOS 运行可视化模拟器

需要 Node.js 22 和系统 C 编译器：

```bash
make -C core test
cd simulator
npm ci
npm run dev
```

打开 [http://127.0.0.1:4173](http://127.0.0.1:4173)。页面通过本地进程调用
`core/build/amtptp-cli`，因此显示的 PTP 字节与驱动使用同一转换实现，不是前端伪造结果。

生产模式：

```bash
cd simulator
npm run build
npm run start
```

## Windows 开发构建

推荐环境：

- Windows 11 x64
- Visual Studio 2022（Desktop development with C++）
- Windows Driver Kit
- NuGet CLI

恢复依赖并构建：

```powershell
nuget restore .\AmtPtpDeviceUsbUm\MagicTrackpad2PtpDevice.vcxproj -PackagesDirectory .\packages
nuget restore .\AmtPtpHidFilter\AmtPtpHidFilter.vcxproj -PackagesDirectory .\packages
msbuild .\AmtPtpDeviceUsbUm\MagicTrackpad2PtpDevice.vcxproj /p:Configuration=Release /p:Platform=x64 /p:ApiValidator_Enable=false
msbuild .\AmtPtpHidFilter\AmtPtpHidFilter.vcxproj /p:Configuration=Release /p:Platform=x64 /p:ApiValidator_Enable=false
```

生成自签名测试包：

```powershell
powershell -ExecutionPolicy Bypass -File .\scripts\windows\New-TestSignedPackage.ps1
```

安装、回滚、Test Mode 和 HVCI 检查见
[Windows 测试指南](docs/windows-testing.md)。

## 驱动签名与证书

最终用户使用 Magic Utilities 一类正式发行驱动时，不需要自己购买证书；发行商必须为
其驱动完成 Windows 认可的签名流程。本项目开发阶段使用自签名测试证书和 Windows
Test Mode。要公开分发，需要组织身份、代码签名凭据和 Microsoft Hardware Dev Center
签名流程；面向零售用户应走 HLK/WHCP，当前 attestation signing 只面向微软定义的测试
场景，单靠仓库中的自签名证书不够。

## 架构与可观测性

- [驱动架构](docs/architecture.md)
- [设备支持矩阵](docs/support-matrix.md)
- [Trace 文件格式](docs/trace-format.md)
- [Windows 测试与诊断](docs/windows-testing.md)
- [两轮对抗性方案审查](docs/adversarial-review.md)

## 来源与许可证

本分支固定基于
[`vitoplantamura/MagicTrackpad2ForWindows@68b31c4`](https://github.com/vitoplantamura/MagicTrackpad2ForWindows/commit/68b31c466f4e2ec8905cf7be44580b01705650f3)，
后者源自 `imbushuo/mac-precision-touchpad`。本项目不会复制或逆向 Magic Utilities 的
专有实现，只实现公开 HID/PTP 行为和独立转换逻辑。

项目继承并遵循 [GPLv2](LICENSE)。
