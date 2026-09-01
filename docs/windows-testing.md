# Windows 11 x64 测试指南

> 安全暂停：当前 KMDF 工程仍包含上游 legacy `Detour.c`。它会依赖私有 HIDClass 布局并
> 改写共享派发表，因此当前只允许编译和只读诊断，不允许生成、签名或安装驱动包。
> 解除暂停还需要完成 VHF 迁移、删除 detour/private-layout 依赖，并用经过审查的专用
> `004C:0324` Bluetooth VHF INF 替换现有 NullDevice/宽泛绑定 INF。

## 1. 当前允许：安装前只读基线

目标必须是原生 AMD64 Windows 11，Trackpad 为 2024 USB‑C 款并通过 Bluetooth 配对。
在普通 PowerShell 中运行：

```powershell
powershell -ExecutionPolicy Bypass -File `
  .\scripts\windows\Collect-MagicTrackpadDiagnostics.ps1
```

采集器会自动寻找同目录或默认 Release 路径下的 `MagicPadHidProbe.exe`。也可以显式指定：

```powershell
powershell -ExecutionPolicy Bypass -File `
  .\scripts\windows\Collect-MagicTrackpadDiagnostics.ps1 `
  -HidProbePath .\MagicPadHidProbe.exe
```

探针以 `CreateFile` desired access `0` 打开候选 HID interface，只读取 attributes、
preparsed data 和 `HidP` caps。它不调用 `ReadFile` / `WriteFile`、不获取或设置 HID report、
不 flush queue，也不读取序列号。探针缺失、15 秒超时或失败时，采集器仍会继续，并在
`hid-caps.json` 写入 `capsUnavailable`；部分 collection 或 caps 子查询失败时状态为
`partial`，只有全部目标 collection 的 descriptor-visible caps 完整时才为 `ok`。

默认输出会隐藏机器名、用户名、Bluetooth 地址、Container ID、interface path 和设备实例
后缀。生成的 zip 仍可能包含驱动清单和事件信息，上传前应人工检查。原始 `0x31` 坐标帧
不会被这个工具收集。

## 2. 基线验收内容

第一份诊断包需要确认：

1. `processArchitecture` / OS architecture 为 AMD64/x64。
2. 真实 Hardware IDs 包含 Bluetooth VID `004c`、PID `0324`。
3. `Col01` / `Col02` 的用途、父子关系、service 和实际 driver stack。
4. 每个 HID top-level collection 的 usage page/usage 和 input/output/feature report length。
5. Secure Boot、HVCI、可用睡眠状态和 Bluetooth adapter/driver 版本。

Windows ARM64 可以模拟 x64 应用，但 x64 kernel driver 不能在 ARM64 kernel 上作为硬件
门禁。参见 [Windows on Arm FAQ](https://learn.microsoft.com/en-us/windows/arm/faq)。VM 的
“暂停虚拟机”也不能代替 guest 内真实 S3/Modern Standby 测试。

## 3. 当前允许：编译验证

在 “x64 Native Tools Command Prompt for VS 2022” 中：

```powershell
nuget restore .\AmtPtpDeviceUsbUm\MagicTrackpad2PtpDevice.vcxproj -PackagesDirectory .\packages
nuget restore .\AmtPtpHidFilter\AmtPtpHidFilter.vcxproj -PackagesDirectory .\packages
msbuild .\AmtPtpDeviceUsbUm\MagicTrackpad2PtpDevice.vcxproj /p:Configuration=Release /p:Platform=x64 /p:ApiValidator_Enable=false
msbuild .\AmtPtpHidFilter\AmtPtpHidFilter.vcxproj /p:Configuration=Release /p:Platform=x64 /p:ApiValidator_Enable=false
msbuild .\tools\windows\MagicPadHidProbe\MagicPadHidProbe.vcxproj /p:Configuration=Release /p:Platform=x64
```

编译成功只说明源代码通过当前 WDK，不代表设备栈拓扑、安全性、HVCI 或硬件行为正确。
CI 在 VHF 迁移完成前只发布诊断工具，不发布 driver CAB。

## 4. VHF 原型发布阻断门禁

安装测试包流程只有满足以下条件后才能恢复：

1. `Detour.c`、`include/Hac.h` 和全局 `MajorFunction` 改写已从工程删除；专用
   `004C:0324` Bluetooth VHF INF 不含旧 NullDevice 或宽泛未验证设备绑定。
2. 物理 Apple collection 的 descriptor/hash 保持不变；filter 对未处理 IRP 透明转发。
3. WinDbg `!devstack` 证明 source filter 位于 `vhf.sys` 上方，顺序符合 INF 设计。
4. 独立 physical `IRP_MJ_READ` 连续 completion，且每设备最多一个 read in flight。
5. VHF child 枚举为独立 Precision Touchpad TLC；caps、HQA、input mode、selective
   reporting callbacks 全部正确完成。
6. D0/S0、禁用/启用、断连、移除时先停止 producer，再 cancel-and-wait；不存在
   completion-after-delete。

如果同一 Col01 上的 source filter + VHF lower-filter 无法可靠共存，必须切换到
“透明物理 filter + 独立 root/software VHF source device”的双栈设计，而不是恢复 detour。

## 5. 架构门禁通过后的功能与稳定性测试

按顺序执行：

1. 单指定位和物理点击。
2. 两指滚动、捏合缩放。
3. 三指/四指 Windows 手势。
4. Palm 与 near-finger rejection。
5. 同时放置 6 指，第 6 个 ID 不抢占前 5 个。
6. `0x90` 电量、充电状态和 UI 展示。
7. 睡眠/唤醒 20 次；Bluetooth 断开/重连 20 次。
8. 连续输入 2 小时；Driver Verifier 无 fault/leak。
9. 设备删除、重新配对、升级、卸载和回滚。

HVCI/Memory Integrity 应保持开启。Code Integrity 拒绝必须作为发布阻断，不把长期关闭
Memory Integrity 当作解决方案。

## 6. 诊断与隐私

VHF 原型完成后，WPP/ETW 只能记录状态、长度、计数、generation 和错误码；Release 不记录
完整坐标或 raw report。带坐标的最小 trace 必须由测试者明确选择，先脱敏再分享。

## 7. 签名与正式发布

自签名只适合隔离测试机，不能公开发布。零售发布需要符合 Microsoft Hardware Dev Center
和当前签名政策，并在本仓库硬件门禁通过后再恢复打包脚本：

- [Driver code signing requirements](https://learn.microsoft.com/en-us/windows-hardware/drivers/dashboard/code-signing-reqs)
- [Driver signing options and best practices](https://learn.microsoft.com/en-us/windows-hardware/drivers/dashboard/driver-signing-offerings)
- [Attestation sign Windows drivers](https://learn.microsoft.com/en-us/windows-hardware/drivers/dashboard/code-signing-attestation)

## 8. 回滚记录要求

未来每次安装前必须保存 `pnputil /enum-drivers /class HIDClass` 和目标设备 stack；卸载时只能
对比确认本项目对应的精确 `oemXX.inf` 后删除，不能把文档中的占位符直接当成命令目标。
