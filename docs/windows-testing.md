# Windows 11 测试指南

## 1. 构建测试包

在 “x64 Native Tools Command Prompt for VS 2022” 中运行：

```powershell
nuget restore .\AmtPtpDeviceUsbUm\MagicTrackpad2PtpDevice.vcxproj -PackagesDirectory .\packages
nuget restore .\AmtPtpHidFilter\AmtPtpHidFilter.vcxproj -PackagesDirectory .\packages
powershell -ExecutionPolicy Bypass -File .\scripts\windows\New-TestSignedPackage.ps1
```

输出目录为 `build\test-package\x64`，包含 INF、SYS、DLL、CAT 和 `.cer`。

## 2. 开启 Test Mode

以下命令会更改系统启动安全状态，必须在管理员终端中由测试者明确执行：

```powershell
bcdedit /set testsigning on
```

如果固件拒绝该命令，需要先在 UEFI 中关闭 Secure Boot，然后重启。测试完成后恢复：

```powershell
bcdedit /set testsigning off
```

再次重启才会生效。Test Mode 不等于禁用所有签名检查；驱动包仍应由测试证书签名。
微软的当前说明见
[Enable loading of test-signed drivers](https://learn.microsoft.com/en-us/windows-hardware/drivers/install/the-testsigning-boot-configuration-option)。

## 3. 信任测试证书并安装

管理员终端：

```powershell
certutil -addstore -f Root .\build\test-package\x64\MagicPadDriverLabTest.cer
certutil -addstore -f TrustedPublisher .\build\test-package\x64\MagicPadDriverLabTest.cer
pnputil /add-driver .\build\test-package\x64\AmtPtpDevice.inf /install
```

先卸载 Magic Utilities、Trackpad++、Apple 2021 driver 以及本驱动旧版本，避免多个 filter
同时绑定。安装后删除 Magic Trackpad 的旧 Bluetooth 配对并重新配对。

## 4. HVCI / Memory Integrity 门禁

先在 Test Mode 下测试 Memory Integrity 开启状态。若 Code Integrity 拒绝加载：

1. 查看 Event Viewer → Applications and Services Logs → Microsoft → Windows →
   CodeIntegrity → Operational。
2. 记录拒绝的文件、Status 和 Policy。
3. 不要长期关闭 Memory Integrity；把兼容性问题作为发布阻断项。

Microsoft 明确说明 HVCI 开启时仍可加载由自建测试证书正确签名的测试二进制；
“未签名二进制”才不受支持。因此先验证签名和 catalog，不把关闭 HVCI 当作默认步骤。

## 5. 正式发布

自签名包不能公开发布。当前 Hardware Dev Center 要求账号关联有效 EV code-signing
certificate；面向零售用户的推荐路线是提交 HLK/WHCP 测试结果并取得 dashboard
signature。Attestation signing 当前只保留给指定测试场景，不用于一般 retail 发布：

- [Driver code signing requirements](https://learn.microsoft.com/en-us/windows-hardware/drivers/dashboard/code-signing-reqs)
- [Driver signing options and best practices](https://learn.microsoft.com/en-us/windows-hardware/drivers/dashboard/driver-signing-offerings)
- [Attestation sign Windows drivers](https://learn.microsoft.com/en-us/windows-hardware/drivers/dashboard/code-signing-attestation)

## 6. 功能门禁

按顺序测试：

1. 单指定位和物理点击
2. 两指滚动、捏合缩放
3. 三指/四指 Windows 手势
4. Palm 与 near-finger rejection
5. 同时放置 6 指，第 6 个 ID 不抢占前 5 个
6. 休眠/唤醒 20 次
7. Bluetooth 断开/重连 20 次
8. 设备删除、重新配对、驱动卸载/回滚

### 2024 USB-C Magic Trackpad Bluetooth bring-up

在安装本项目驱动前后各运行一次只读采集脚本，以确认 `VID 004c / PID 0324` 的真实枚举、
驱动栈和错误事件：

```powershell
powershell -ExecutionPolicy Bypass -File `
  .\scripts\windows\Collect-MagicTrackpadDiagnostics.ps1
```

安装测试驱动后，可附加 15 秒 WPP/ETW 元数据采集：

```powershell
powershell -ExecutionPolicy Bypass -File `
  .\scripts\windows\Collect-MagicTrackpadDiagnostics.ps1 `
  -TraceSeconds 15
```

脚本不会切换设备模式、发送 Feature Report、重新配对或更改驱动。默认会隐藏机器名、
用户名、Bluetooth 地址、Container ID 和设备实例后缀。生成的 zip 仍可能包含本机驱动
清单和事件信息，上传前应人工检查。原始 `0x31` 坐标帧不由此脚本收集。
脚本同时保存 `powercfg /a`；如果 VM 不支持 S3 或 Modern Standby，宿主机的
“暂停虚拟机”不能作为 Windows guest 的睡眠/唤醒验收。

运行脚本时还应确认 `processArchitecture` 为 `AMD64`。Windows ARM64 可以模拟
x64 应用，但内核驱动必须与 OS 架构匹配，所以 ARM64 VM 不能验收 x64 KMDF
二进制。参见 [Windows on Arm FAQ](https://learn.microsoft.com/en-us/windows/arm/faq)。

当前目标和证据状态见 [支持矩阵](support-matrix.md)。

## 7. WPP/ETW 诊断

仓库提供 `AmtPtpHidFilter\AmtPtpHidFilter.wprp`。管理员终端：

```powershell
wpr -start .\AmtPtpHidFilter\AmtPtpHidFilter.wprp!AmtPtpHidFilter -filemode
# 复现问题
wpr -stop .\magicpad-filter.etl
```

Verbose 转换事件只记录触点数量、Scan Time、Button 和准入/抑制 ID mask，不记录完整坐标
或原始 report。用构建产物中的 PDB/TMF 解码 WPP 事件。

## 8. 回滚

先在 Device Manager 或 `pnputil /enum-drivers /class HIDClass` 中确认本项目对应的
`oemXX.inf`，再执行：

```powershell
pnputil /delete-driver oemXX.inf /uninstall
```

必须把 `oemXX.inf` 替换为确认过的精确目标，不能照抄占位符。删除测试证书：

```powershell
certutil -delstore Root "MagicPad Driver Lab Test"
certutil -delstore TrustedPublisher "MagicPad Driver Lab Test"
```
