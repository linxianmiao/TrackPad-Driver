# MagicPad Native PTP Driver

将 Apple Magic Trackpad 2 / USB‑C Magic Trackpad 的原始多点触控报告转换为
Windows 11 原生 Precision Touchpad（PTP）报告。Windows 接收的是标准 PTP 触点，
滚动、缩放、三指/四指手势仍由系统手势栈处理。

> 当前状态：本机原生手势已验证，发布前的稳定性验收仍待完成。
> Bluetooth 新 source 使用专用 L2CAP + VHF；旧驱动打包保持隔离，
> 新 source 原型仅通过专用入口开展明确授权的本机测试。仓库不包含可公开分发的 Microsoft 签名。

新的 [Bluetooth L2CAP → VHF 驱动原型](docs/native-ptp-source.md) 已实现于 `AmtPtpSource`，
包含多点收发、PTP 功能报告和触点释放；已在本机接收真实触点并向 Windows 上报。
用户已确认单指移动、按压点击、双指滚动/缩放、三指任务视图/桌面/窗口切换和四指桌面切换正常。
睡眠唤醒、断连重连、卸载恢复及 Verifier 等稳定性测试仍待完成。

电量直接显示在 [Windows 设置的蓝牙设备列表](docs/battery-display.md) 中；驱动每分钟
读取实际百分比，断连或读数过期时清除。无需单独弹窗、托盘程序或 Magic Utilities。

新开发者或 AI 接手请先读 [AI 接手手册](docs/ai-handoff.md)：代码地图、实际实现与目标架构的
差异、诊断/验证方法、已知坑点及下一阶段任务。

## 已实现

- Bluetooth 优先：已识别 Apple Bluetooth VID `0x004c`、PID `0x0265` / `0x0324`；
  `0x0324` 目前处于实机 bring-up，不代表驱动已支持
- USB‑C Magic Trackpad PID `0x0324` 的现有 USB 路径
- 原生 PTP HID 描述符与 5 触点、50 字节 Input Report 合约
- Apple `0x31` Report 的显式小端解析，不依赖 C 位域布局
- 稳定的 5 触点准入、Palm Confidence、Near Finger、按压锁定与 Scan Time
- 驱动和 macOS 模拟器共用同一份无浮点、无动态分配的 C17 转换核心
- 本地 Web 模拟器：手动拖拽、手势预设、时间线和 trace 导入/导出
- WPP/ETW 元数据诊断；默认不记录原始触点字节
- Windows x64 只读 HID caps 探针；不读取/写入 report，不采集序列号

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

## Windows 只读 bring-up

在原生 Windows 11 x64 上配对 2024 USB‑C Magic Trackpad 后，先运行采集器：

```powershell
powershell -ExecutionPolicy Bypass -File `
  .\scripts\windows\Collect-MagicTrackpadDiagnostics.ps1
```

如果同目录或默认构建路径存在 `MagicPadHidProbe.exe`，采集器会额外导出 descriptor-visible
HID caps；探针不存在时仍会完成 PnP、Bluetooth、系统和事件诊断。默认输出会隐藏机器名、
用户名、Bluetooth 地址、Container ID 和设备实例后缀。此步骤不安装驱动、不切换设备模式。

## Windows 编译验证（不可安装）

推荐环境：

- Windows 11 x64
- Visual Studio 2022（Desktop development with C++）
- Windows Driver Kit
- NuGet CLI

恢复依赖并编译，仅用于发现 WDK 编译错误：

```powershell
nuget restore .\AmtPtpDeviceUsbUm\MagicTrackpad2PtpDevice.vcxproj -PackagesDirectory .\packages
nuget restore .\AmtPtpHidFilter\AmtPtpHidFilter.vcxproj -PackagesDirectory .\packages
msbuild .\AmtPtpDeviceUsbUm\MagicTrackpad2PtpDevice.vcxproj /p:Configuration=Release /p:Platform=x64 /p:ApiValidator_Enable=false
msbuild .\AmtPtpHidFilter\AmtPtpHidFilter.vcxproj /p:Configuration=Release /p:Platform=x64 /p:ApiValidator_Enable=false
msbuild .\tools\windows\MagicPadHidProbe\MagicPadHidProbe.vcxproj /p:Configuration=Release /p:Platform=x64
```

`AmtPtpHidFilter` 当前仍包含待删除的 legacy detour；编译成功不授权生成测试签名包或安装。
迁移计划和后续硬件门禁见 [Windows 测试指南](docs/windows-testing.md)。

## 驱动签名与证书（VHF 原型通过后）

最终用户使用 Magic Utilities 一类正式发行驱动时，不需要自己购买证书；发行商必须为
其驱动完成 Windows 认可的签名流程。本项目开发阶段使用自签名测试证书和 Windows
Test Mode。要公开分发，需要组织身份、代码签名凭据和 Microsoft Hardware Dev Center
签名流程；面向零售用户应走 HLK/WHCP，当前 attestation signing 只面向微软定义的测试
场景，单靠仓库中的自签名证书不够。

## 架构与可观测性

- [驱动架构](docs/architecture.md)
- [设备支持矩阵](docs/support-matrix.md)
- [Windows 蓝牙实机传输检查](docs/bluetooth-transport-findings.md)
- [Trace 文件格式](docs/trace-format.md)
- [Windows 测试与诊断](docs/windows-testing.md)
- [对抗性方案审查与架构更正](docs/adversarial-review.md)

## 实施顺序

1. 用只读 PnP/HID caps 采集确认真实 `004c:0324` collection、report lengths 和栈。
2. 建立透明 physical `IRP_MJ_READ` + VHF virtual PTP 的 x64 原型。
3. 删除 `Detour.c` / 私有 HIDClass 布局依赖，把 Feature/Input helpers 接到 VHF callbacks。
4. 完成 `0x31` fixture、`0x90` 电量、断连重连和 D0 生命周期。
5. 通过 HVCI、Driver Verifier、睡眠/重连、卸载回滚后，才恢复签名包流程。

## 来源与许可证

本分支固定基于
[`vitoplantamura/MagicTrackpad2ForWindows@68b31c4`](https://github.com/vitoplantamura/MagicTrackpad2ForWindows/commit/68b31c466f4e2ec8905cf7be44580b01705650f3)，
后者源自 `imbushuo/mac-precision-touchpad`。本项目不会复制或逆向 Magic Utilities 的
专有实现，只实现公开 HID/PTP 行为和独立转换逻辑。

项目继承并遵循 [GPLv2](https://www.gnu.org/licenses/old-licenses/gpl-2.0.html)。
