# AmtPtpSource 本机测试准备

这是独立 Bluetooth source 原型的本机测试流程，用户已明确授权该次测试。
旧 `New-TestSignedPackage.ps1`、Detour/private-layout 工程保持隔离。
测试包不会上传 GitHub；CI 仍只编译 unsigned source。原型已加载，手势尚未实机验收。

2026-09-08 运行状态：Secure Boot 已关闭，当前内核 TESTSIGN 位已确认，C、D 盘的
BitLocker 保护均为开启。已安装的当前版本为 0.2.0.2，专用 source 和 VHF 子设备
状态正常，`InputMode=3`。两条蓝牙通道和模式命令写入成功，设备应答缺失；现允许在
该超时之后等待真实 interrupt 触点数据。当前有蓝牙数据到达，但没有 `A1 31` 证据。
用户暂时不能触摸设备，下一步是用户操作时同时观察 `TouchPackets`、`ModeEnabled`
和 `Reports`，再验证双指、三指、四指功能。

## 已完成

- 重新构建 `AmtPtpSource` x64 Release，代码分析和 Universal DDI 验证通过。
- InfVerif `/w` 与 Inf2Cat 检查通过；仅包含精确的 `004c:0324` Bluetooth HID service ID。
- SYS 嵌入签名、CAT 签名、CAT 对 SYS/INF 的成员校验通过。
- 专用 RSA/SHA256 测试证书有效期 30 天，私钥不可导出，留在 CurrentUser\My。
  公钥证书已导入 LocalMachine\Root 和 LocalMachine\TrustedPublisher。
- 本机预检：ThinkBook 16 G6+ IMH，Windows 11 x64；Secure Boot 开启，VBS/HVCI 未运行。
  C、D 盘均完全加密且保护开启。系统盘的暂停操作由进入固件脚本在重启前执行并复查。

实际结果记录在忽略的 `diagnostics/source-test-<Action>.json` 中。
包内 `manifest.json` 记录精确哈希、签名证书与构建来源；不得把源码提交号当作 SYS 哈希。

## 准备与两次重启

普通 x64 PowerShell 生成包：

```powershell
.\scripts\windows\New-MagicPadSourceTestPackage.ps1 -EnableHardwareTest
```

管理员 PowerShell 使用生成的目录做信任和完整校验：

```powershell
.\scripts\windows\Prepare-MagicPadSourceTest.ps1 -Action TrustPackage -PackagePath <包目录>
```

首次重启进入 BIOS：`-Action EnterFirmware` 会暂停系统盘 BitLocker 保护，
限定两次 Windows 重启后恢复；确认暂停成功才调用 `shutdown /r /fw /t 0`。
默认先等待 45 秒供用户保存工作，不强制关闭应用。它不读取恢复密码、不清除 TPM、
不删除保护器，也不关闭磁盘加密。重启调用失败时尝试立即恢复保护。

BIOS 中找到 Secure Boot，将其改为 Disabled，保存并返回 Windows。
只需要改变 Secure Boot 开关；不要清除 TPM、删除安全启动密钥或切换 Legacy/CSM。
BIOS 菜单因固件版本可能不同，通常在 Security 或 Boot 页面。

返回 Windows 后，先重新检查 Secure Boot 确已关闭，再以管理员身份运行：

```powershell
.\scripts\windows\Prepare-MagicPadSourceTest.ps1 -Action EnableTestSigning
```

该步骤暂挂系统盘保护一次重启，并设置 `bcdedit /set testsigning on`；随后还需重启一次。
脚本不自动安装驱动。第二次启动后核实当前内核的测试签名状态，
以及 C、D 盘保护状态。必要时用 `-Action ResumeProtection` 恢复系统盘保护。

## 安装、验证与恢复

保留可用的笔记本键盘和内置触摸板。安装前重新校验包哈希与签名、系统状态、
目标数量必须为一个，并记录当前绑定为 `hidbth.inf`。通过 PnPUtil 添加包并安装，
核实实际绑定；若系统因驱动排名保持 HidBth，再审查针对精确硬件 ID 的更新。
不要安装到所有 Bluetooth 设备，也不要改写蓝牙类的全局过滤器。

首次加载后：读取 `Read-MagicPadSourceStatus.ps1`，检查 `InputMode=3`、两通道连接、
有效触点包和上报计数；核对设备树，再让用户验证双指、三指和四指动作。
随后才开展禁用/启用、睡眠、重连、卸载和针对本驱动的 Verifier 检查。

恢复时定位**此次实际安装的 AmtPtpSource 对应 oem*.inf**，确认 provider、
原 INF 名称和精确设备绑定后执行 `pnputil /delete-driver <该 oem.inf> /uninstall`，
再 `/scan-devices`，检查重新绑定到 Microsoft HidBth 并测试单指移动。
不要删除其他 oem 包，不用 `/force` 掩盖正在使用或卸载失败。
如果原型使启动失败，先关掉外部 Trackpad，再进入安全模式停用 AmtPtpSource 服务并移除该包。

结束测试、移除原型后，关闭 TESTSIGNING 并恢复 Secure Boot；这些启动变更前同样需
处理 BitLocker 暂停并在最后确认恢复。确认没有包依赖测试证书后，按此次 manifest
的精确 thumbprint 移除其两处信任和 CurrentUser\My 私钥；不要按泛化名称删除其他证书。

依据：[Microsoft 测试签名启动要求](https://learn.microsoft.com/en-us/windows-hardware/drivers/install/the-testsigning-boot-configuration-option)、
[BitLocker 有限次数暂停](https://learn.microsoft.com/en-us/windows-server/administration/windows-commands/manage-bde-protectors)、
[Lenovo Secure Boot 指引](https://support.lenovo.com/us/en/solutions/nvid500424-disable-and-enable-secure-boot-in-bios-lenovo-support-quick-tips)。
