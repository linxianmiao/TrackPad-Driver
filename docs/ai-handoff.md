# AI 接手手册：MagicPad Native PTP Driver

本文面向接手开发的 AI，记录代码现状、设计边界、验证方法和下一步，不是安装指南。

> 新增实现见 [原生 PTP source](native-ptp-source.md)：`AmtPtpSource` 已实现独立 L2CAP
> profile + VHF 原型，并通过编译和合成协议测试；已在本机加载、枚举 PTP 并接通蓝牙，
> 已接收真实触点，用户确认移动、按压点击、双指滚动/缩放、三指和四指系统手势正常。
> 生命周期及发布稳定性验收仍待完成。
> 后续已实现 [系统蓝牙列表电量](battery-display.md)：新 source 定期读取实际 `0x90`
> 电量，0.2.0.6 通过设备属性直接发布给 Windows；状态接口 v5 可诊断发布结果。
> 本机已收到 100% 并验证多轮主动查询，用户确认系统蓝牙列表显示 100%。
> 不再使用独立弹窗/托盘。充电状态字节尚未解释；生命周期稳定性仍须验收。
> 本地 Web 模拟器已移除；保留共享 C 核心、`amtptp-cli` 和自动化测试。
> 下文标为 legacy / 历史的架构和待办仅供旧工程对照，不能再用来概括整个仓库。
> 新原型改变 HidBth 的持有关系，当前实现及实机证据以本节链接的文档为准。

原始 legacy 快照日期：2026-09-07；当时基线为 `master@13af581`，实现提交为 `5f3a01d`。
后续接手时先检查分支、工作区和提交记录，不把历史快照当成最新状态。

> 后续 Windows 实机证据见 [蓝牙传输检查](bluetooth-transport-findings.md)：
> 已移除 Magic Utilities、确认系统驱动单指移动；但 Col01 的用户态读访问被拒绝，
> 原始 descriptor-visible caps 没有 `0x31`。原型 A/B 的物理采集前提仍需验证。

## 1. 先读这一页

- 用户目标：让自己的 **2024 USB-C Magic Trackpad，通过蓝牙连接 Windows x64**，
  使用 Windows 原生 Precision Touchpad 手势。界面暂不优先，先打通协议和安全驱动路径。
- 用户已确认 Windows 是 x64；具体 OS build、真实 HID collection/caps、蓝牙栈信息尚待诊断包确认。
  项目验收环境是 Windows 11 原生 AMD64，不是 ARM64 Windows 中模拟运行的 x64 应用。
- 主目标为 Bluetooth `VID 004c / PID 0324`；同设备 USB 路径是 `05ac:0324`，不能混为一条链路。
- 继续在本仓库开发，参考开源方案，不另起一个 Magic Utilities 克隆项目。
  不从用户曾提供的 Magic Utilities EXE 复制专有代码、资源或授权逻辑。
- 目前保留共享 C 核心、命令行调试工具、只读诊断工具和 legacy 驱动编译验证；
  新 `AmtPtpSource` 已实现专用 L2CAP + VHF，本机基本功能与双/三/四指手势已验证，
  生命周期及发布稳定性仍待验收。
- legacy 驱动仍禁止打包；新 source 的隔离测试流程见
  [本机测试准备](source-hardware-test.md)，不得把测试签名包当成正式发行包。
- Git 规则：改动必须走独立分支和 PR 合并，禁止直接向 `main` / `master` 推送改动。

### 已实现、未实现、未验证

| 范围 | 源码现状 | 不应据此声称 |
| --- | --- | --- |
| `core` | 裸 `0x31` 解码、稳定五指准入、PTP 转换和固定 50 字节序列化 | 仅凭合成测试即可证明所有真实设备输入兼容 |
| Legacy KMDF Bluetooth | 链接 core；有生命周期防护；仍使用 detour 和旧 HID IOCTL 链路 | 该旧工程已采用受支持的 VHF 架构 |
| USB UMDF | 保留上游独立解析/转换路径，含 `0324` ID | USB 已接入共享 core，或 USB 成功即证明蓝牙成功 |
| `AmtPtpSource` / VHF | 专用 L2CAP function driver 和 VHF 已实现，本机基本手势已验证 | 生命周期、其他机器或正式发行签名已验证 |
| Native CLI | JSON-lines 标准输入/输出调用共享 core；保留 session 与 reset | 可移植测试通过即 Windows 能枚举 PTP |
| 只读诊断 | 枚举 PnP、descriptor-visible HID caps、系统和事件 | 已采到原始 `0x31` 坐标报告或验证设备模式切换 |
| Legacy 电量 | 旧控制 IOCTL 主动获取 `0x90` 并读取 byte 2；连续输入路径丢弃 `0x90` | 旧入口可代表新 source 的电量实现；新实现证据见本文开头 |
| 安装/发布 | legacy 打包入口无条件阻断；新 source 有本机测试签名流程；CI 不上传驱动安装包 | Release 编译、HQA blob 或诊断 artifact 等于正式签名驱动 |

## 2. 推荐阅读顺序和代码地图

按下面顺序读，不要先从庞大的旧 USB 驱动或 UI 开始：

1. [README](../README.md)、[架构](architecture.md)、[对抗性审查](adversarial-review.md)：
   理解为什么保留转换核心，但必须替换 detour 传输架构。
2. [核心接口](../core/include/amtptp_core.h)、[核心实现](../core/src/amtptp_core.c)、
   [核心测试](../core/tests/test_core.c)：先掌握输入、状态与输出合约。
3. [KMDF Driver.c](../AmtPtpHidFilter/Driver.c)、[Device.c](../AmtPtpHidFilter/Device.c)、
   [Input.c](../AmtPtpHidFilter/Input.c)、[Hid.c](../AmtPtpHidFilter/Hid.c)、
   [Queue.c](../AmtPtpHidFilter/Queue.c)：核对实际收发和生命周期。
4. [Windows 测试指南](windows-testing.md)、[采集器](../scripts/windows/Collect-MagicTrackpadDiagnostics.ps1)、
   [探针说明](../tools/windows/MagicPadHidProbe/README.md)：确认下一阶段需要什么实机证据。
5. [CI](../.github/workflows/build.yml)、[源码合约检查](../scripts/check-driver-contracts.mjs)、
   [打包阻断脚本](../scripts/windows/New-TestSignedPackage.ps1)：了解可验证范围与安全门禁。

| 路径 | 职责与入口 |
| --- | --- |
| [core/include/amtptp_core.h](../core/include/amtptp_core.h) | 类型、常量、options、每设备 session、公开 API |
| [core/src/amtptp_core.c](../core/src/amtptp_core.c) | 唯一共享解码/转换/序列化实现；无浮点、无动态分配 |
| [core/tools/amtptp_cli.c](../core/tools/amtptp_cli.c) | JSON-lines 标准输入/输出适配器；持久 session |
| [AmtPtpHidFilter](../AmtPtpHidFilter) | 当前 Bluetooth KMDF legacy filter；设备上下文在 `include/Device.h` |
| [Detour.c](../AmtPtpHidFilter/Detour.c)、[Hac.h](../AmtPtpHidFilter/include/Hac.h) | 待移除的共享派发表改写和私有 HIDClass 布局依赖 |
| [AmtPtpDeviceUsbUm](../AmtPtpDeviceUsbUm) | 旧 USB UMDF；`InputInterrupt.c` 的 Type5 分支自行解析 MT2 位域 |
| [AmtPtpControlPanel](../AmtPtpControlPanel) | 旧 WinForms 设置界面，通过注册表/控制 IOCTL 配置；不是当前开发重点 |
| [AmtPtpSource](../AmtPtpSource) | 新 Bluetooth L2CAP + VHF source；独立于 legacy filter |
| [MagicPadHidProbe.cpp](../tools/windows/MagicPadHidProbe/MagicPadHidProbe.cpp) | x64 只读 SetupAPI/HidP caps 探针；不做 report IO |
| [build](../build) | **受版本控制的 INF 和历史构建脚本目录，不是可整目录删除的临时产物** |

## 3. Legacy 数据链和历史迁移设计

当前 source 数据链见 [原生 PTP source](native-ptp-source.md)。以下仅描述保留的旧工程
和早期 collection 原型，不是新 `AmtPtpSource` 的工作路径。

### Legacy KMDF 实现

`DriverEntry` → `PtpFilterEvtDeviceAdd` → `PtpFilterCreateDevice` 创建 filter。
`PtpFilterSelfManagedIoInit` 仍调用 detour：读取私有 HIDClass 布局，修改底层共享
`DRIVER_OBJECT->MajorFunction[IRP_MJ_INTERNAL_DEVICE_CONTROL]`。

输入大致为：

```text
上层 HID read → 手工 HidReadQueue → work item
  → 下层 IOCTL_HID_READ_REPORT → completion 校验/拆包
  → amtptp_decode_mt2 → amtptp_convert_ptp → amtptp_serialize_ptp
  → 完成一个上层待处理 read
```

`Queue.c` 还拦截 descriptor/features；`Hid.c` 实现现有 GET/SET_FEATURE helpers。
已有修补包括 single-flight read、generation 校验、工作项重投递、D0 producer gate、
取消/等待、同步请求超时和返回状态检查。这些是 **旧传输上的防护**，不是 VHF 实现，
也不使全局派发表改写成为可发布方案。

### 历史 VHF collection 原型候选

```text
物理 Apple HID collection（保持原 descriptor）
  → 独立 file object 的持续 IRP_MJ_READ
  → 每设备转换 core/session
  → VhfReadReportSubmit
  → 独立虚拟 PTP child → Windows 手势栈
```

计划新增 `AmtPtpHidFilter/VhfDevice.c/.h`、`PhysicalHid.c/.h`、`PtpReports.c/.h`，
链接 `VhfKm.lib`；此旧工程迁移方案没有落地。实际的新 VHF 实现位于
`AmtPtpSource/Vhf.c`，不是这些历史候选文件。

先验证同栈原型 A：physical source filter 在 `vhf.sys` 上方，专用 INF 明确顺序。
如果同一 Col01 栈无法可靠工作，转为原型 B：透明物理 filter + 独立 root/software
VHF source device。两者都需实机证据；不得用恢复 detour 绕过问题。

迁移时保留以下约束：

- 物理 collection 的 descriptor 不改写，未处理 IRP 透明转发。
- 物理输入使用持续 `IRP_MJ_READ`，不以轮询 GetInputReport 替代流式输入。
- 每设备最多一个物理 read 在途；convert、reset 和状态变更有明确串行边界。
- 停止 producer → cancel-and-wait / rundown → reset 或删除，禁止 completion-after-delete。
- VHF 异步 feature 操作恰好完成一次；`VhfDelete(..., TRUE)` 的等待和执行级别按架构文档处理。
- caps、完整 HQA、input mode、selective reporting 都要接到新的 VHF callbacks 并验证。

## 4. 共享核心：输入、状态、输出

通常调用顺序：

```c
amtptp_default_options(&options);
amtptp_reset_session(&session);
/* 每帧逐步检查返回值，前一步成功后才继续。 */
amtptp_decode_mt2(bytes, length, &raw);
amtptp_convert_ptp(&session, &options, &raw, &frame);
amtptp_serialize_ptp(&frame, output, capacity, &output_length);
```

- `amtptp_decode_mt2` 名字里的 mt2 不代表只服务 Lightning 型号；当前 `0324` 路径也复用它，
  但协议和参数仍需真实帧验证。仅接受裸 `0x31`，长度 `4 + 9 × N`，`N ≤ 16`。
- 显式小端解码、13-bit 有符号坐标，避免用 C 位域布局定义线缆协议。
- session 含 `admitted_mask`、`suppressed_mask`、`previous_button`、`locked_contacts[2]`。
  锁定历史只有两个槽，不要描述为完整五指位置历史。
- 先保留仍出现的已准入 ID，再分配新 ID；第六指被抑制后，必须从输入消失才能重新取得准入资格。
- `palm_rejection != 0` 时，`finger == 6` 清除 Confidence；`ignore_near_fingers != 0` 时，
  near 状态清除 Tip。坐标平移、Y 翻转并截断；pressure/size 锁定继承旧语义，
  不能仅凭字段名推断“大于阈值就锁住”。
- Scan Time 为毫秒时间戳 × 10，按 16 位回绕；不是任意全局帧序号。

输出 `0x05` 固定 50 字节；正常 convert 结果的未使用触点槽为零，serialize 写出全部五槽：

| 字节偏移（从 0 开始） | 内容 |
| --- | --- |
| 0 | Report ID `0x05` |
| 1–45 | 5 个 9 字节槽：flags 1 字节 + Contact ID 4 字节 LE + X 2 字节 LE + Y 2 字节 LE |
| 46–47 | Scan Time，16-bit LE |
| 48 | Contact Count，最多 5 |
| 49 | Button |

每槽 flags 的 bit 0 是 Confidence、bit 1 是 Tip Switch，其余为零；byte 49 仅 bit 0 为 Button。
修改序列化时必须同时核对 [PTP 触点描述符](../AmtPtpHidFilter/include/Metadata/MagicTrackpad2.h)、
[完整 descriptor 组合](../AmtPtpHidFilter/include/Metadata/StaticHidRegistry.h)和测试；
[WindowsHID.h](../AmtPtpHidFilter/include/Metadata/WindowsHID.h)另含配置 TLC 和 HQA。
不能仅改某一端结构体大小。

### 相同 core 不等于相同默认行为

Native CLI 固定使用 `amtptp_default_options`，没有 options 配置 API；
legacy KMDF 在 `Input.c` 从 `PtpFilterReadSettings` 读到的注册表设置组装 options。
以下是无注册表覆盖时的差异：

| 选项 | Core / CLI 默认 | Legacy KMDF 设置默认 |
| --- | --- | --- |
| `stop_pressure` | `0xffffffff` | `0` |
| `ignore_button_finger` | `0` | `1` |
| `palm_rejection` | `1` | `0` |

只有 **原始输入、options、session 初始状态和帧顺序都一致**，才能要求共享 core 结果逐字节一致。
若比较 KMDF 最终上报字节，还需对齐 convert 后的 `PtpReportTouch` / `PtpReportButton`
selective reporting 状态（或保持两者均开启）。USB UMDF 尚未使用这个 core，不能将该保证
扩展到 USB 旧路径。

## 5. 命令行调试与 trace 的用途

在仓库根目录执行；需要系统 C 编译器、make、Node.js 22：

```bash
make -C core clean
make -C core test all
node scripts/check-driver-contracts.mjs
node --test scripts/tests/*.test.mjs
```

不要并行执行 clean 与 test/all。本地 Web 服务、前端和对应 npm 依赖已移除，
以下 CLI 直接调用共享核心，不需要浏览器或 HTTP 服务。

CLI 冒烟测试：

```bash
printf '%s\n' '{"requestId":"reset","command":"reset"}' '{"requestId":"empty","reportHex":"31000000"}' | ./core/build/amtptp-cli
```

CLI 返回 JSON decoded、PTP 字段及 `ptpReportHex`。它是简单 JSON-lines 适配器，不是通用 JSON
解析库；reset 使用示例中的紧凑 `"command":"reset"` 形式。

[Trace 格式](trace-format.md) 是 `magicpad-trace/v1` JSON，不是 Windows ETL：
格式说明继续保留，但 CLI 不直接导入整份 trace。复现时先 reset，再按帧序将每个
`rawReportHex` 作为 CLI 的 `reportHex` 提交。可选 `ptpReportHex` 只作为期望值，
需要调用方显式比较；保存 fixture 时保留原始来源证据。

核心测试使用合成数据；当前未见已提交的 `0324` 蓝牙真实输入 fixture。
新增 fixture 应附型号、transport、采集条件、最小原始帧和期望结果，去掉可识别信息，
只在测试者明确同意后保留必要坐标，不上传长时间操作轨迹。

## 6. Windows 只读诊断：获取、运行、交回什么

`magicpad-x64-read-only-diagnostics` 是 GitHub Actions 的 **artifact 名称**，不是包管理器包名，
也不是可安装驱动。打开本仓库 [Actions](https://github.com/linxianmiao/TrackPad-Driver/actions)，
选择成功运行，在 Artifacts 下载同名文件并解压。它包含：

```text
MagicPadHidProbe.exe
Collect-MagicTrackpadDiagnostics.ps1
SHA256SUMS.txt
```

也可使用 GitHub CLI（将 RUN_ID 替换为实际成功运行编号）：

```bash
gh run download RUN_ID -R linxianmiao/TrackPad-Driver -n magicpad-x64-read-only-diagnostics
```

历史成功运行：[33470017755](https://github.com/linxianmiao/TrackPad-Driver/actions/runs/33470017755)，
对应实现提交 `5f3a01d`；artifact 有保留期限，过期后选新成功运行或构建探针，不能假设永久可下载。

在原生 Windows x64 上蓝牙配对触控板，核对下载文件哈希后，在解压目录的普通 PowerShell 运行：

```powershell
powershell -ExecutionPolicy Bypass -File .\Collect-MagicTrackpadDiagnostics.ps1
```

若从源码目录运行，用 `scripts\windows\Collect-MagicTrackpadDiagnostics.ps1`；
探针搜索顺序为显式 `-HidProbePath`、脚本同目录、仓库默认 Release 构建路径。

默认输出到当前目录 `magicpad-diagnostics-yyyyMMdd-HHmmss/` 和同名 `.zip`，拒绝覆盖已有路径。
重点检查：

| 文件 | 接手者要确认的问题 |
| --- | --- |
| `system.json`、`power-capabilities.txt` | 真实 OS/process architecture、OS build、HVCI、Secure Boot、睡眠状态 |
| `devices.json`、`device-XX-pnputil.txt` | `004c:0324` Hardware IDs、Col01/Col02、父子关系、service、stack |
| `bluetooth-adapters.json` | 蓝牙适配器、驱动版本和日期 |
| `hid-caps.json` | 各 top-level collection usage、input/output/feature lengths、HidP caps、探针 hash |
| `hid-driver-inventory.txt`、`driver-services.json` | 已有 HID 驱动/服务，是否存在可能干扰的旧栈 |
| `system-events.json`、`code-integrity-events.json`、`setupapi-matches.txt` | 安装、连接、Code Integrity 线索 |
| `manifest.json`、`hashes.sha256`、`*-error.txt` | schema、采集结果完整性、隐私选项、查询失败原因 |

注意：`manifest.target.osArchitecture` 是固定目标字符串 `x64`，不能证明宿主 OS 是 x64；
应以 `system.json` 实际字段为依据。权限不足导致的查询失败也不等于 Secure Boot/HVCI 已关闭。

探针以 desired access `0` 打开接口，只读 attributes/preparsed data/HidP caps，不读写 HID report。
`ok` 仅表示本次探针发现的所有目标 collection 的 descriptor-visible caps 完整；`partial` 是部分可用；
`capsUnavailable` 包括缺少探针、15 秒超时或查询失败；`targetNotFound` 表示未发现目标接口。
这些状态都不是“触控板兼容/不兼容”的结论。没有 probe 时，其他 PnP 采集仍继续。

默认分享采集器输出，而不是探针独立 stdout（含完整 interfacePath/instanceId）。
采集器默认脱敏，仍需人工检查 zip；不要默认加 `-IncludeSensitiveIdentifiers`。
保持 `TraceSeconds = 0`：可选 WPR/ETL 会启动跟踪，**二进制 ETL 不经过文本脱敏**，
自定义 WPR profile 也不能被视为强制 metadata-only。该工具不会提供原始 `0x31` fixture，
后续真实触点采集需另行设计并明确取得测试者同意。

## 7. Windows 构建、CI 和已知工具链坑

在 VS 2022 x64 开发者命令环境安装 C++、WDK 和 NuGet CLI 后，按
[构建命令](windows-testing.md#3-当前允许编译验证)恢复并编译。探针输出为：

```text
tools/windows/MagicPadHidProbe/build/x64/Release/MagicPadHidProbe.exe
```

当前 NuGet SDK/WDK 版本固定为 `10.0.26100.6584`。兼容性修复不要轻易回退：

- legacy CI 使用 `windows-2022` 和 MSBuild `[17.0,18.0)`；MSBuild 18 曾寻找不存在的
  DriverKit `*.18.0.dll`，不是源码错误。探针不依赖 WDK，单独用 `windows-latest`。
- 共享核心头在 `_KERNEL_MODE` 下用 `ntdef.h` / `SIZE_T`，用户态才用标准 CRT 头。
  不要无条件重新引入 `stdint.h`，否则 MSVC 用户态 CRT 与 WDK kernel CRT 冲突。
- Windows 编译使用 `ApiValidator_Enable=false`；通过编译不代表通过 API 合规认证。
- 旧 WDK 项目构建可能有内部测试签名步骤；CI 的“no package output”表示不交付安装包，
  不能推断所有本地构建过程完全不涉及签名。

[当前 CI](../.github/workflows/build.yml) 有四个 job：

1. `portable-core`：C 核心/source 测试、源码合约、Node 22 工具和 CLI 测试。
2. `diagnostics-x64`：等待 portable job，构建并上传只读诊断 artifact。
3. `native-source-x64`：编译新的 Bluetooth VHF source，运行代码分析，不签名或打包。
4. `legacy-compile`：USB/KMDF 的 x64、ARM64 和旧控制面板 AnyCPU 编译；上传源码 tar。

触发器为 pull request、`feature/**` push 和手动 dispatch。仅推送 `codex/**` 不触发 CI，
创建 PR 才触发；当前也没有 `master` push 触发。PR 源码包的 SHA 可能是 GitHub 合成 merge commit。

核心测试覆盖单触点、错误输入、palm/near、五指稳定准入、防重排抢占及 2000 帧随机不变量。
源码合约检查主要是静态模式断言，包括 HQA 完整性、Feature 长度、探针禁用 report IO、
超时/状态处理和打包隔离。两者均不能证明 Windows 枚举、VHF、竞态、电源、HVCI 或蓝牙实机通过。

本地验证入口见第 5 节；每次交接应重新运行并记录结果。旧 Web 模拟器的构建和 npm
依赖审计已不属于当前检查范围，移除它们不代表 Windows 驱动安全或稳定性已通过验收。

## 8. Legacy 工程坑点和安全边界

这些是旧工程交接发现，本文不修改对应实现；新 source 的状态、输入模式、电量和
生命周期限制以 [原生 PTP source](native-ptp-source.md) 为准：

- `PtpInputOn` 在 `Hid.c` 的 input-mode Feature 中被赋值并 reset session，但当前全仓没有
  读取它来控制输入输出；不能声称 mouse/PTP mode 切换已生效。selective touch/button flags
  则确实在 `Input.c` 应用，后续迁移时分别补验证。
- wrapper parser 支持 `0x02` mouse 前缀和裸 `0x31`；`0xF7` 合包、`0xFC/0xFE` 分片、
  `0x90` 和未知报告在连续输入路径被丢弃，没有重组实现。
- 电量旧入口在 `ControlDevice.c` 的 `IOCTL_PTPFILTER_GET_BATTERY`，不是新 VHF 电量通路。
  迁移时需核对返回长度、`0x90` 状态/电量字段以及 `0324` 的真实行为。
- `PtpFilterConfigureMultiTouch` 的 BT 初始化先发继承的 F2 haptic，再发 F1 payload `02 01`。
  ID 白名单包含 `0324` 不等于 haptic 初始化已验证；不要无证据复制所有旧初始化到新传输。
- `Diagnostics.c` 存在旧连续读实验函数，但未找到初始化调用；主输入入口是 `Input.c`。
  不要和只读 `MagicPadHidProbe` 混淆。
- `New-TestSignedPackage.ps1` 当前在入口无条件 `throw`；后续 VHF 文件/依赖检查是第二道防线，
  不是删掉 throw 就能发布的承诺。禁止通过 `-SkipBuild` 复用旧驱动二进制。
- 历史 `build/make.bat`、`build/make_win10.bat` **不受上述 throw 保护**，包含签名/打包及清理
  result 的命令，不应运行。未来迁移需要一并清理这些旁路。
- 现有 AMD64/ARM64 INF 仍有广泛历史设备绑定及 Col02 → NullDevice，不是专用 VHF INF。
  [支持矩阵](support-matrix.md) 中“确认后可给 Col02 绑定 Null Device”的旧表述不能解除
  [Windows 测试指南](windows-testing.md)的专用 INF / 移除 NullDevice 门禁。
- 不把 VM 暂停当成 guest S3/Modern Standby；原生 x64 kernel、蓝牙直通/真实设备栈和目标测试
  条件缺一不可。macOS 上的可移植测试或 Windows ARM64 编译不能替代目标实机验收。

## 9. 历史原型阶段待办（不能作为当前进度）

以下为 collection 原型阶段的原始顺序。专用 L2CAP + VHF source 已实现并取得本机
基本手势证据；不要重复将“实现最小 VHF 原型”列为尚未开始。当前剩余验收见
[本机测试准备](source-hardware-test.md) 和 [原生 PTP source](native-ptp-source.md)。

1. **取得只读基线。** 请用户提供已检查隐私的 Windows x64 蓝牙诊断 zip；明确 Col01/Col02、
   report lengths、stack、蓝牙驱动、OS build/HVCI。缺失字段记录为未知，不猜 collection 用途。
2. **实现最小 VHF 原型。** 基于实际栈选择 A/B，删除 detour/private layouts；新建专用
   `004c:0324` Bluetooth INF，物理读、VHF child 和 feature callbacks 分层实现。
   本阶段仍保持打包暂停，不把新增文件或编译通过当成架构门禁完成。
3. **取得架构证据。** 按 Windows 指南核验 `!devstack`、物理 descriptor 不变、单在途 read、
   PTP TLC、Feature completion 和停止/删除顺序。若验证需进入当前暂停范围内的安装/签名，
   先说明隔离测试方案、恢复措施并取得新的明确授权，不自行解除阻断。
4. **补协议证据与回归。** 经测试者同意采集最小 `0324` 真帧；对齐 options/session 后添加
   golden tests，验证模式切换、六指准入、palm/near、合包/分片实际需求、电量和充电。
5. **做硬件稳定性门禁。** 单指/点击、两指滚动缩放、三/四指系统手势、睡眠 20 次、重连 20 次、
   连续输入 2 小时、Verifier、禁用/启用、升级/卸载/回滚。HVCI 保持开启，失败则阻断发布。
6. **最后讨论打包和 UI。** 遵照指南区分隔离测试与零售发布；确认全部所需证据后才调整
   打包门禁，不用自签名冒充公开发行资格。签名政策届时查 Microsoft 官方最新要求。

每阶段交付应写清：改了什么、验证命令和结果、未验证什么、是否需要用户实机操作。
当前没有足够证据把 `0324` Bluetooth 从 `bring-up` 改为 `verified`。

## 10. 来源、Git 和接手提示词

仓库：[linxianmiao/TrackPad-Driver](https://github.com/linxianmiao/TrackPad-Driver)，默认分支 `master`。
历史来源链：固定基于
[`vitoplantamura/MagicTrackpad2ForWindows@68b31c4`](https://github.com/vitoplantamura/MagicTrackpad2ForWindows/commit/68b31c466f4e2ec8905cf7be44580b01705650f3)，
后者源自 [imbushuo/mac-precision-touchpad](https://github.com/imbushuo/mac-precision-touchpad)，
继承 [GPLv2](https://www.gnu.org/licenses/old-licenses/gpl-2.0.html)。不能描述为全部从零自研，也不能将参考项目支持范围当成本仓库验证结果。
公开协议参考与固定 Linux 源码链接见 [设备支持矩阵](support-matrix.md)。

历史定位点：`5f3a01d` 修复 core 的 kernel CRT 边界；`13af581` 是此前 PR #1 的 merge commit。
本文不自动更新这些 SHA。开始工作时执行：

```bash
git status --short --branch
git remote -v
git log -5 --oneline
```

先保留现有未提交改动，再从核对后的 `origin/master` 建独立 `codex/<task>` 分支。
`origin` 是用户仓库，`upstream` 是参考仓库；使用 gh 时显式传
`-R linxianmiao/TrackPad-Driver`，避免 PR/Actions 操作落到 upstream。
提交、推送和合并按用户授权执行；合并前核对 PR base 为 `master`、head 为本次分支、CI 结果对应当前 head。
禁止直接向 `master` 推送改动，不需要为正常提交关闭打包合约检查。

可直接发给下一位 AI：

```text
请接手 linxianmiao/TrackPad-Driver，先阅读 docs/ai-handoff.md，并核对当前分支、
README、架构、Windows 测试指南与实际源码。目标是 Windows x64 + 2024 USB-C
Magic Trackpad 的蓝牙 004c:0324，界面暂不优先，沿用本仓库并参考开源实现。

先汇报已实现、仅设计、缺实机验证三类状态。AmtPtpSource 已实现专用 L2CAP + VHF，
本机基本操作与双/三/四指手势已验证；生命周期和发布稳定性仍待验收。
core、amtptp-cli 和只读诊断工具保留，本地 Web 模拟器已移除。
legacy Bluetooth detour 工程保持隔离，不要与新 source 混为同一数据链。

下一步优先核对 native-ptp-source、source-hardware-test 和 battery-display 中的实机证据。
实现任务另行确认范围，按新 source 的架构和硬件门禁推进，不安装当前 legacy 包，
不删除打包 throw 绕过限制，不默认关闭 HVCI，不把合成测试当作硬件证据。
涉及安装、签名或新采集方式时先说明风险、隔离和回滚方案，取得所需授权。

所有改动走独立分支和 PR，禁止直接向 main/master 推送。做完给出验证结果、剩余风险
和下一步；没有实机条件时明确标记未验证，不宣称兼容性完成。
```
