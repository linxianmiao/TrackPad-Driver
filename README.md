# MagicPad Native PTP Driver

简体中文 | [English](README.en.md)

让 **2024 USB-C 款 Apple Magic Trackpad 通过蓝牙连接 Windows 11 x64**，使用系统原生 Precision Touchpad（PTP）手势，并在蓝牙设置中显示电量。

> 当前为开发测试版 `AmtPtpSource 0.2.0.6`，没有可直接安装的正式微软签名发行包。
> 下文是自行构建、测试签名和本机安装流程，会修改证书信任和启动设置；不适合不愿承担测试风险的日常主力机。

## 系统与设备要求

| 项目 | 要求与验证范围 |
| --- | --- |
| Windows | 原生 **Windows 11 x64（Intel / AMD）**。INF 最低匹配 build 22000；本机测试环境为 build 26100，不代表所有 Windows 11 版本均已验证。 |
| Trackpad | **2024 USB-C 款 Magic Trackpad**，通过 **Bluetooth** 连接，蓝牙 VID/PID 为 `004c:0324`。 |
| 未支持或未验证 | Windows 10、Windows ARM64、虚拟机、旧款 Lightning / 第一代 Trackpad、USB 有线输入，不在本版安装支持范围内。USB-C 是设备代际，不表示本驱动使用 USB 连接。 |
| 测试启动配置 | 当前流程使用 Secure Boot 关闭、TESTSIGNING 开启的配置。桌面显示“测试模式”是预期现象。 |
| HVCI / 内存完整性 | 本机验收时未运行，开启状态尚未验证。若已开启，请先停在只读诊断阶段，不要为了绕过兼容性问题关闭它。 |
| 安装条件 | 管理员权限、可用的备用键盘/鼠标；修改启动配置前，确认可以取回 BitLocker / 设备加密恢复密钥。不要把密钥上传到仓库或日志。 |

新驱动只匹配以下蓝牙 HID 服务，而不是所有 Apple 或 Bluetooth 设备：

```text
BTHENUM\{00001124-0000-1000-8000-00805f9b34fb}_VID&0001004C_PID&0324
```

## 已验证功能与限制

- 单指移动、按压点击、双指滚动和缩放。
- 三指任务视图 / 显示桌面 / 窗口切换，以及四指虚拟桌面切换；具体动作由 Windows 触摸板设置决定。
- 蓝牙设置列表显示实际电量，约每 60 秒查询一次，无需额外托盘程序。

完整手势验收对应 `0.2.0.2`；当前 `0.2.0.6` 复验了触控和电量显示（本机读数为 100%）。睡眠唤醒、断连重连、卸载恢复、Driver Verifier 和长期稳定性仍待验收。电量变化、充电状态及其他电脑/蓝牙适配器不能视为已验证。

## 如何安装（仅本机测试）

请按顺序操作，任何一步失败都先停止。下列脚本和命令应在**仓库根目录、64 位 PowerShell** 中运行；标为“管理员”的步骤需要提升权限。

### 1. 准备构建环境

在 Windows 上安装：

- Git、Node.js 22、NuGet CLI，并确保命令在 PATH 中。
- Visual Studio 2022 / Build Tools 2022（MSBuild 17），C++ 桌面开发、v143 x64/x86 Spectre 缓解库和 Windows Driver Kit 构建集成组件。
- 项目使用 NuGet 固定的 SDK / WDK `10.0.26100.6584`；不要仅安装另一版本的系统 WDK 后跳过依赖恢复。

打开 VS 2022 的 **Developer PowerShell（x64）**，克隆源码并恢复依赖：

```powershell
git clone --branch master --single-branch https://github.com/linxianmiao/TrackPad-Driver.git
if ($LASTEXITCODE -ne 0) { throw 'Clone failed; stop here.' }
cd TrackPad-Driver
nuget restore .\AmtPtpSource\AmtPtpSource.vcxproj -PackagesDirectory .\packages
if ($LASTEXITCODE -ne 0) { throw 'Dependency restore failed; stop here.' }
```

生成专用测试包：

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\scripts\windows\New-MagicPadSourceTestPackage.ps1 -EnableHardwareTest
```

这一步重新构建 `AmtPtpSource`，校验 INF，生成 CAT，并签署 SYS/CAT；会创建或复用本机测试证书（新证书有效期 30 天，私钥不可导出）。**不会安装驱动、修改启动配置或加入机器信任库。**

保存最后打印的完整包目录：`diagnostics\source-test-YYYYMMDD-HHMMSS`。包内包含 SYS、INF、CAT、`SourceTest.cer` 和 `manifest.json`；不要手动修改包内文件，也不要把 CI 的只读诊断 EXE 当成驱动安装器。

### 2. 准备测试启动配置（管理员）

先保存工作，备好备用输入设备和加密恢复密钥。若已安装 Magic Utilities、Trackpad++ 或其他替代 Trackpad 驱动，使用其正常卸载流程，并先确认微软自带驱动下的基本指针功能正常。

在管理员 64 位 PowerShell 中进入仓库根目录，用上一步**实际打印的绝对目录**设置变量；示例路径必须替换。每次重新打开窗口或重启后，都要重新赋值：

```powershell
$magicPadPackage = 'C:\path\to\TrackPad-Driver\diagnostics\source-test-YYYYMMDD-HHMMSS'
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\scripts\windows\Prepare-MagicPadSourceTest.ps1 -Action TrustPackage -PackagePath "$magicPadPackage"
```

`TrustPackage` 校验包内哈希、证书和签名，将该包的公钥证书导入本机 Root / TrustedPublisher，再验证 CAT 成员。只信任自己构建并确认来源的包。

先只读检查启动和磁盘状态：

```powershell
Confirm-SecureBootUEFI
manage-bde -status
```

如果 Secure Boot 仍开启，以下命令会暂停受保护系统盘的 BitLocker 保护（限定两次 Windows 重启），**默认等待 45 秒后重启进入固件**：

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\scripts\windows\Prepare-MagicPadSourceTest.ps1 -Action EnterFirmware
```

在 UEFI 中仅关闭 Secure Boot，保存并返回 Windows。不要清除 TPM、安全启动密钥或切换 Legacy / CSM。若 Secure Boot 原本已关闭，跳过 `EnterFirmware`。若脚本无法确认加密或固件状态，请停止，不要绕过检查。

返回 Windows 后，再以管理员身份在仓库根目录运行：

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\scripts\windows\Prepare-MagicPadSourceTest.ps1 -Action EnableTestSigning
```

该步骤设置 TESTSIGNING，并对受保护系统盘暂停一次重启的保护，**不会自动重启**。保存工作后手动重启；启动后检查测试模式，并用 `manage-bde -status` 确认所有加密盘的保护状态。仅当已加密系统盘仍处于暂停状态时，运行：

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\scripts\windows\Prepare-MagicPadSourceTest.ps1 -Action ResumeProtection
```

脚本只恢复系统盘，其他加密盘需分别核实；不要对未加密的系统盘无条件执行该动作。测试模式与重启要求见 [Microsoft 说明](https://learn.microsoft.com/en-us/windows-hardware/drivers/install/the-testsigning-boot-configuration-option)，保护暂停机制见 [BitLocker 文档](https://learn.microsoft.com/en-us/windows-server/administration/windows-commands/manage-bde-protectors)。

### 3. 确认设备并安装（管理员）

在 Windows 设置中通过蓝牙配对 Trackpad，不通过 USB 线测试输入。重新打开管理员 64 位 PowerShell，进入仓库根目录，并重新设置上面的 `$magicPadPackage`。

只读定位唯一、精确匹配的蓝牙 HID 服务：

```powershell
$magicPadHardwareId = 'BTHENUM\{00001124-0000-1000-8000-00805f9b34fb}_VID&0001004C_PID&0324'
$magicPadTargets = @(Get-PnpDevice -PresentOnly |
    Where-Object { $_.InstanceId -like 'BTHENUM\*' } |
    Where-Object {
        $magicPadIds = (Get-PnpDeviceProperty -InstanceId $_.InstanceId `
            -KeyName DEVPKEY_Device_HardwareIds -ErrorAction Stop).Data
        @($magicPadIds) -contains $magicPadHardwareId
    })
if ($magicPadTargets.Count -ne 1) { throw 'Expected exactly one target Bluetooth HID service.' }
$magicPadInstanceId = $magicPadTargets[0].InstanceId
pnputil /enum-devices /instanceid "$magicPadInstanceId" /drivers
```

首次安装应先确认目标由 Microsoft HidBth / `hidbth.inf` 驱动，且微软驱动下基本移动正常。若数量不是一个、身份不明或仍绑定其他第三方驱动，先停止。不要把这里的完整设备实例 ID 直接发布到公开日志。

安装前重新校验同一个包，再安装：

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\scripts\windows\Prepare-MagicPadSourceTest.ps1 -Action TrustPackage -PackagePath "$magicPadPackage"
if ($LASTEXITCODE -ne 0) { throw 'Package verification failed; installation aborted.' }
pnputil /add-driver "$magicPadPackage\AmtPtpSource.inf" /install
if ($LASTEXITCODE -ne 0) { throw 'Review the PnPUtil result, including any restart request, before continuing.' }
Get-PnpDeviceProperty -InstanceId $magicPadInstanceId -KeyName DEVPKEY_Device_Service,DEVPKEY_Device_DriverInfPath
```

记录实际安装的发布名称 `oemNN.inf`，并确认目标服务是 `AmtPtpSource`。若提示需要重启，先重启，再重新定位目标并核对服务。PnPUtil 将包加入 Driver Store **不等于已经绑定设备**；它不会强制替换排名更高的驱动。若仍为 HidBth 或安装报错，保留输出和诊断后停止，不要强制安装到其他设备或改写全局蓝牙/HID 过滤器。详见 [PnPUtil 文档](https://learn.microsoft.com/en-us/windows-hardware/drivers/devtest/pnputil-command-syntax)。

### 4. 验证

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\scripts\windows\Read-MagicPadSourceStatus.ps1
```

触摸 Trackpad 后，应看到 `Connected=1`、`ModeEnabled=1`、`InputMode=3`，`TouchPackets` / `Reports` 计数增长，且 `InvalidPackets=0`。`sourceNotPresent` 表示没有找到新驱动接口，不代表设备型号一定不兼容。

打开 Windows“设置 → 蓝牙和设备 → 触摸板”测试移动、点击和双/三/四指手势；在蓝牙设备列表检查电量。电量按分钟查询，界面可能有刷新延迟。计数和设置页出现设备不能代替实际手势测试。

## 如何卸载与恢复

保留备用输入设备。在管理员 PowerShell 中用 `pnputil /enum-drivers` 查找并核对：原始名称 `AmtPtpSource.inf`、提供商 `MagicPad Native PTP Project`，以及上一步目标实际使用的发布名称。

仅把下面的 `oemNN.inf` 换成**已经核实的那个包**，不要使用通配符或 `/force`：

```powershell
pnputil /delete-driver oemNN.inf /uninstall
pnputil /scan-devices
```

按 Windows 提示重启，确认设备恢复为 Microsoft HidBth，并测试基本指针。该恢复流程尚未完成实机验收，不能保证任何故障都可自动恢复。若原型造成启动问题，先关闭外部 Trackpad，再通过 Windows 恢复环境/安全模式处理，不要批量删除蓝牙驱动。参见 [微软卸载说明](https://learn.microsoft.com/en-us/windows-hardware/drivers/install/using-device-manager-to-uninstall-devices-and-driver-packages)。

确认原型已移除、没有其他测试驱动依赖 TESTSIGNING 后，在处理好受保护系统盘的有限次数 BitLocker 暂停后执行：

```powershell
bcdedit /set testsigning off
```

重启使设置生效，再通过 Windows 高级启动进入 UEFI 恢复 Secure Boot，最后确认各加密盘保护均已恢复。恢复 Secure Boot 不使用本项目的 `EnterFirmware` 动作（该动作会在 Secure Boot 已关闭时拒绝执行）。确认没有其他包使用该测试证书后，按 `manifest.json` 的精确指纹清理其 Root / TrustedPublisher 信任及 CurrentUser\My 私钥；不要按泛化名称批量删除证书。

## 开发与更多资料

`AmtPtpSource` 是新的蓝牙 L2CAP + VHF 驱动；旧 `AmtPtpHidFilter`、`AmtPtpDeviceUsbUm` 和控制面板仅保留作历史对照，不是本 README 的安装目标。**不要使用旧 `build/make*.bat` 或 `New-TestSignedPackage.ps1` 安装旧包。**

跨平台核心和工具测试（需 C 编译器、make、Node.js 22）：

```sh
make -C core test all
node scripts/check-driver-contracts.mjs
node --test scripts/tests/*.test.mjs
```

保留 `core/build/amtptp-cli` 用于命令行协议调试。核心/工具测试和 CI 不能替代 Windows 实机验收。

- [原生驱动实现与限制](docs/native-ptp-source.md)
- [本机测试记录与恢复细节](docs/source-hardware-test.md)
- [电量查询与系统显示](docs/battery-display.md)
- [AI 接手手册](docs/ai-handoff.md)（含历史快照，阅读时以新 source 文档和实际代码为准）
