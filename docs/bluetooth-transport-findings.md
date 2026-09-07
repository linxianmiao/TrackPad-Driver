# 004c:0324 系统蓝牙 HID 的传输检查

实测日期：2026-09-07。环境：Windows 11 build 26200，原生 AMD64，2024 USB-C
Magic Trackpad，USB 已拔除。移除 Magic Utilities 3.1.5.3 后，目标绑定 Microsoft
`hidbth.inf` 10.0.26100.8737，Upper/LowerFilters 均为空。用户确认单指移动正常。
以下是本机结果，不代表所有 Windows/固件组合。

## 为什么必须先检查传输

共享 core 接受 `0x31`，并不代表系统 HID collection 能把 `0x31` 交给它。
原型 A/B 都依赖物理 collection 读路径，这个前提仍未通过验证。

| 项目 | Col01 | Col02 |
| --- | --- | --- |
| TLC Usage Page / Usage | 0x0001 / 0x0002，Mouse | 0xFF00 / 0x0014，厂商自定义 |
| Input IDs / 最大长度 | 0x02 / 8 字节 | 0x90 / 3 字节 |
| Feature IDs / 最大长度 | 0x55 / 65 字节 | 无 / 0 字节 |
| 用户态 metadata open | 成功 | 成功 |
| 用户态 GENERIC_READ open | 失败，Win32 5 | 成功 |
| 用户态 GENERIC_WRITE open | 成功 | 成功 |
| 本地初始化已声明 input | 0x02 成功 | 0x90 成功 |
| 本地初始化 input 0x31 | 0xC0110010 | 0xC0110010 |

完整只读探针返回 `ok`。这些长度是 HIDP_CAPS 的声明值，不是采样帧长度。
`HidP_InitializeReportForID` 仅使用 preparsed data 和本地内存，不向设备发包。
Col01 的 F1 和已声明的 0x55 feature 都返回 0xC0110010；因此 feature 初始化检查
不能单独证明设备是否接受某个 feature，仍须独立验证实际 transport。
该状态在 WDK `hidpi.h` 中为 `HIDP_STATUS_REPORT_DOES_NOT_EXIST`。

Microsoft 文档说明系统以独占方式打开鼠标 collection，但允许零读写权限的元数据句柄。
本机读句柄结果与之相符；**用户态读访问失败不等于所有内核 filter 方案都失败**。
另一个独立问题是当前 descriptor-visible caps 没有声明 `0x31` 或 F1。
增加用户态 read buffer 大小本身不能证明这些报告会被路由到 collection。
参见 [HID architecture](https://learn.microsoft.com/en-us/windows-hardware/drivers/hid/hid-architecture)。

## Linux 一手实现提供了什么证据

固定源码版本 `abdf623ddb75b24659018d3952d8f61937306ae5` 的 `hid-magicmouse.c`：

- Bluetooth Trackpad 2 / USB-C 的模式命令为 `F1 02 01`；
- 使用 raw transport feature request 发送，不能等同于 Windows collection Feature API；
- probe 主动注册 `TRACKPAD2_BT_REPORT_ID`（0x31），而非只依赖默认报告描述符；
- 注释指出切到多点模式后不再提供相对移动数据。

参见 [模式切换实现](https://github.com/torvalds/linux/blob/abdf623ddb75b24659018d3952d8f61937306ae5/drivers/hid/hid-magicmouse.c#L719)
及 [输入报告注册](https://github.com/torvalds/linux/blob/abdf623ddb75b24659018d3952d8f61937306ae5/drivers/hid/hid-magicmouse.c#L886)。
这解释了为什么不能只复制三个模式字节，就认为 Windows 的原始触点链路已打通。

## 可重复检查

先按 [交接手册](ai-handoff.md#6-windows-只读诊断获取运行交回什么)取得完整只读探针，
或者本地构建探针。无需安装本仓库驱动，也无需管理员权限：

```powershell
pwsh -NoProfile -File .\scripts\windows\Test-MagicTrackpadTransport.ps1 `
  -HidProbePath .\tools\windows\MagicPadHidProbe\build\x64\Release\MagicPadHidProbe.exe `
  -OutputPath .\diagnostics\transport-preflight.json
node scripts/analyze-transport-preflight.mjs .\diagnostics\transport-preflight.json
```

输出目录应提前存在；脚本拒绝覆盖文件。只接收 physical Bluetooth `004c:0324`，
不将 USB 或重写 VID 的 collection 当作原始蓝牙证据。探针子进程有 15 秒超时。
脚本短暂尝试 metadata/read/write 句柄，全部关闭；没有 report I/O、模式切换或坐标采集。
interface path 只保留在进程内，不写入输出。

分析器的 `collectionPathBlocked` 只表示所检查的用户态 collection 路径不满足前置条件。
`requiresRuntimeValidation` 也不代表模式切换或 VHF 已通过。缺失/部分数据返回
`inconclusive`，不能把没有找到接口解读为硬件不兼容。

完整系统诊断可加 `-PresentOnly` 跳过离线历史设备枚举；默认仍保持原有范围。
不需要复制修改采集脚本：

```powershell
pwsh -NoProfile -File .\scripts\windows\Collect-MagicTrackpadDiagnostics.ps1 `
  -PresentOnly -HidProbePath .\MagicPadHidProbe.exe
```

## 下一阶段的实现边界

先证明接收路径，再切换模式。按以下顺序推进：

1. 核对合法的 kernel physical read/filter 位置能否观察到未声明的报告，以及设备独占和
   HIDClass 路由约束；不能用修改共享 MajorFunction 或私有结构来绕过。
2. 若 collection 路径不可用，评估专用 Bluetooth profile/source driver 通过官方 BRB
   接口持有原始 L2CAP 通道，再向 VHF 输出。Microsoft 提供
   [Bluetooth driver stack/BRB 接口](https://learn.microsoft.com/en-us/windows-hardware/drivers/bluetooth/using-the-bluetooth-driver-stack)
   和 [L2CAP client connection](https://learn.microsoft.com/en-us/windows-hardware/drivers/bluetooth/creating-a-l2cap-client-connection-to-a-remote-device)
   文档。它们证明有受支持的接口，不证明 Apple HID profile 的通道、重连及控制事务已实现。
3. 该候选会改变 `HidBth` 的持有关系；它不是已经批准的透明 filter 设计。先评审设备专用
   绑定、SDP/PSM、控制/中断通道、取消与断连、系统鼠标回退，再进行安装验证。
4. 输入通道可用后再验证 F1、0x31 的帧长度/计数，随后接共享 core 与 VHF。
   新采集默认只保留 report ID/长度/计数；真实坐标 fixture 仍按交接手册单独取得同意。

本轮没有执行 F1，没有实现/安装新 kernel driver，没有解除签名与打包暂停。
这份检查更正了“物理 collection 可以直接供给 0x31”的未验证假设，不能作为终点替代
多点协议和 VHF 原型。

## 本机代码验证

在原生 Windows x64 安装 Visual Studio 2022 Build Tools 后，已完成：

- MSVC C17 `/W4 /WX` 编译共享 core 并通过其测试；
- MSVC 构建 CLI，修复 `strcpy` 编译诊断与 Windows 下 `_IOLBF, 0` 导致的运行时异常；
- 持久管道测试确认 stdin 尚未关闭时，reset 和转换请求都能收到 JSON 回复；
- Release x64 完整 HID 探针构建、运行，并复现上述 collection 检查结果；
- PowerShell 7 与 Windows PowerShell 5.1 的脱敏回归验证，保留 hardware ID，
  隐去设备实例后缀；
- transport 分析测试及 driver source contracts 通过。

这些验证只覆盖 core、用户态工具和源码合约；不等于 legacy kernel 编译、
驱动加载或真实多点手势通过。CI 加入相同的 Windows core/CLI 与双 PowerShell 检查。
