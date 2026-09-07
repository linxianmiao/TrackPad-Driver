# 原生手势驱动原型：Bluetooth L2CAP → VHF

`AmtPtpSource` 已实现新的数据链，目标是 2024 USB-C Magic Trackpad 的 Bluetooth
`004c:0324`。原生 Windows 手势需要实际加载后验证；编译与合成测试不等于手势已可用。

## 实现

```text
BTHENUM 的特定 Apple HID service
  → AmtPtpSource（设备专用 KMDF function driver）
    → 自有 L2CAP control 0x11 / interrupt 0x13
    → HIDP SET_REPORT: 53 F1 02 01
    → HIDP DATA input A1 + Apple 31
    → 共享解码、稳定五指准入、显式触点释放
    → VhfReadReportSubmit（50 字节 PTP 05）
    → Windows 双指滚动/缩放、三指/四指手势
```

这是前次传输检查中专用 profile/source 候选的实现，改变了原先透明 collection filter
的设计前提。它在目标 HID service 上替代 HidBth；**它不是透明 filter**。其他设备的
HID service 不匹配其 INF。它不修改物理描述符，也不改写任何共享驱动派发表。
原有 `AmtPtpHidFilter` 保持隔离，新工程不链接它的 detour、私有结构或收发代码。

- `Driver.c`：PnP、自管 I/O 生命周期和只含计数的状态接口。
- `Bluetooth.c`：通过公开 BTH profile interface 提交 BRB。运行时再次验证 HID service
  UUID、VidType、VID/PID；地址仅保存在内存中。所有连接要求链路加密。
- `Vhf.c`：VHF 创建/删除，GET/SET_FEATURE 回调和默认输入缓冲。每个 feature operation
  完成一次；回调不发蓝牙请求。
- `core/src/amtptp_source.c`：HIDP 帧校验、完整 256 字节 HQA、输入模式、触摸/按钮开关、
  2–5 指报告与触点释放。手势识别交给 Windows，没有鼠标快捷键模拟。

每个设备只有一个生产线程、一个 BRB 请求和一个接收缓冲；不会并发重复投递 read。
停止时设置事件，取消在途请求并等待 completion，再关闭通道、等待线程结束、删除 VHF。
中断通道使用流式 ACL read；500 ms 无数据超时用于处理停止/配置，不是 GET_INPUT_REPORT
轮询。断连释放保留旧 Contact ID/位置、清除 Tip/Button，再发空帧，随后重置 session。
新旧 ID 更替时也先上报离开的触点，避免五指满额时丢失 lift。

Linux 一手实现记录：某些 Apple 设备在模式切换生效时仍回应 invalid-report-ID。
本实现只容许成功 `00` 和这个确切应答 `02`，其他错误拒绝；`ModeEnabled` 必须在收到
结构合法的 `A1 31` 后才置位，不能由成功写入或握手推断。

## 构建和测试

需要 VS 2022 C++ Build Tools、Windows Driver Kit Build Tools 集成组件、最新 v143
x64/x86 Spectre 库、NuGet，以及项目固定的 SDK/WDK `10.0.26100.6584`。

```powershell
nuget restore .\AmtPtpSource\AmtPtpSource.vcxproj -PackagesDirectory .\packages
msbuild .\AmtPtpSource\AmtPtpSource.vcxproj /p:Configuration=Release /p:Platform=x64 /p:RunCodeAnalysis=true
```

项目选择 x64 编译宿主，避免 NuGet x64 WDK 的分析插件被错误地从 x86 目录加载。
构建结束额外调用匹配架构的 ApiValidator，验证导入的 DDI；KMDF 库与 INF 均固定为 1.15。
输出为 `AmtPtpSource/build/x64/Release/AmtPtpSource.sys`；默认不签名、不生成 catalog，
不把 `.inf.in` 当成安装 INF。CI 和旧驱动打包入口继续保持这一限制。
用户明确授权的本机测试可以使用单独的
[`New-MagicPadSourceTestPackage.ps1`](../scripts/windows/New-MagicPadSourceTestPackage.ps1)，
流程见[本机测试准备](source-hardware-test.md)。它只重新构建并签名 `AmtPtpSource`。

Portable core：`make -C core test all`。Windows native-tools 命令行：

```cmd
cl /nologo /std:c17 /W4 /WX /O2 /Icore\include core\src\amtptp_core.c core\src\amtptp_source.c core\tests\test_source.c /Focore\build\ /Fecore\build\test-source.exe
core\build\test-source.exe
```

测试覆盖功能报告和边界、完整 HQA、输入模式、2–5 指、满额 ID 更替、全部抬起、
断连、独立触摸/按钮开关、未知/电量报告不打断手势、短包、重复 ID、HIDP header 和
描述符位数。它们验证传给 Windows 的数据合约，不验证 Windows 手势识别结果。

## 实机门禁和当前限制

2026-09-07 本机 Release x64 编译、MSVC `/W4 /WX`、代码分析、Universal DDI 验证和 source 单元测试通过。
主机报告 `UEFISecureBootEnabled=1`；常规构建产物仍未签名。
用户随后明确授权关闭 Secure Boot、启用测试签名和重启进入本机测试。
专用测试包已通过 InfVerif `/w`、Inf2Cat、SYS/CAT 签名验证及 catalog 成员验证，
本机已信任专用测试证书。2026-09-08 已关闭 Secure Boot，核实运行内核的 TESTSIGN
位已开启，C、D 盘保护均已恢复，并安装加载了专用原型。PnP 状态为 OK；VHF 的触摸板
和 MTConfig 子设备正常枚举，Windows 设置了 `InputMode=3`。已接收真实触点，用户随后
确认单指移动、按压点击、双指滚动、捏合/张开缩放正常。第二组测试中，用户也确认
三指任务视图/显示桌面/窗口切换及四指虚拟桌面切换正常。生命周期验收仍待完成；
没有采集原始坐标 fixture。

安装前需评审 `.inf.in` 的 function-driver 替换和 VHF lower-filter 拓扑。专用原型的
签名/安装须单独启用，旧驱动打包入口仍保持阻断。必须有可操作的备用输入设备，且明确
恢复 Microsoft HidBth 的步骤。移除原型包并重新扫描设备应恢复原绑定；这一恢复步骤也
必须实测，不能仅凭 INF 的精确匹配推断。

首次加载必须证明：

1. 只替换目标 Bluetooth HID service；WinDbg `!devstack` 确认 source → vhf → BTHENUM。
2. 公开 BTH interface、两条加密 L2CAP 通道、模式应答与有效 `A1 31` 接收。
3. VHF 独立 PTP TLC、Windows GET/SET_FEATURE、`InputMode=3`、有效输入持续提交。
4. 实际双指滚动/缩放及三/四指系统手势，包含满额触点切换和按钮。
5. 睡眠/唤醒、禁用/启用、断连/重连、卸载，配合 Driver Verifier 检查取消、引用和移除。

加载后可运行 `pwsh -File scripts/windows/Read-MagicPadSourceStatus.ps1` 读取连接、输入模式、
收包/触点包/上报计数和最后错误。它只调用新驱动的固定状态 IOCTL，不返回设备地址或坐标。
`sourceNotPresent` 表示没有发现活动 source 接口，不能解读成硬件不兼容。

状态 v2 为 88 字节，保留原有 52 字节前缀并增加 transport stage、最后失败 BRB 的
NTSTATUS/Bluetooth status/type、成功打开通道与模式写入计数、握手字节（缺失为 -1）。
新读取器也能读取 v1 驱动；v1 没有传输诊断尾部，不能解释其补零字段。
Stage 值：0 idle、1 control open、2 interrupt open、3 mode write、4 handshake read、
5 input read、6 close、7 retry、8 stopped。失败历史不会被成功关闭通道覆盖。

首次加载发现两通道打开和模式写入均成功，但 control handshake read 返回
`STATUS_IO_TIMEOUT (0xC00000B5)`。版本 0.2.0.2 在这个确切超时分支继续等待 interrupt
输入，避免反复拆连接；这是一项由本机观察引入的原型处理，不是声称 Linux 已证明该行为。
超时缓冲不会被解析，明确错误应答仍拒绝；只有真实有效的 `A1 31` 才设置 `ModeEnabled`。
更新后的首轮空闲观察尚无触点。用户实际触摸后，先观察到 579 个有效触点报告，
在用户确认基本操作正常后增至 `TouchPackets=Reports=2887`，`ModeEnabled=1`、
`LastStatus=0`、`InvalidPackets=0`，`Reconnects` 保持为 1。此证据确认真实 `A1 31`
到 PTP 的数据链，结合第一轮用户反馈确认基本移动/点击与双指滚动/缩放。
第二轮三/四指测试也获得用户“正常”的确认，之后读取到
`TouchPackets=Reports=4261`（比前次增加 1374），解析错误仍为 0，`Reconnects` 仍为 1。
基本指针和双/三/四指手势的本机功能验收通过，但这不等于断连恢复或长期稳定性已通过。
`FailureStage=4` 仍保留最初缺失握手的历史，不代表当前输入失败。

当前实现仅主动建立连接，每两秒尝试重连；没有注册全局 HID PSM server，因此设备主动
重连与其他 HID profile 共存尚待实机验证。固定使用标准 HID PSM 和 basic L2CAP，
没有实现 SDP 协商、HIDP 分片重组或非目标型号。超长/非完整帧不会送入核心。
关闭通道失败时保留句柄并终止重连，状态接口报告错误；这条错误路径及 callback 引用
释放必须通过移除/Verifier 测试后，才能作为可分发驱动。不能把通过编译当作消除这些风险。

## 一手依据

- [Microsoft BthEcho sample](https://github.com/microsoft/Windows-driver-samples/tree/main/bluetooth/bthecho)：公开 profile interface、BRB、设备信息和连接生命周期。
- [Microsoft VHF](https://learn.microsoft.com/en-us/windows-hardware/drivers/hid/virtual-hid-framework--vhf-)：source driver、lower filter、feature callbacks、默认 buffering 与删除顺序。
- [Microsoft L2CAP ACL transfer](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/bthddi/ns-bthddi-_brb_l2ca_acl_transfer)：流式 ACL 请求及长度/超时。
- [Bluetooth assigned PSMs](https://www.bluetooth.com/wp-content/uploads/Files/Specification/HTML/Assigned_Numbers/out/en/index-en.html)：HID control/interrupt。
- [Linux hid-magicmouse 模式命令及应答例外](https://github.com/torvalds/linux/blob/abdf623ddb75b24659018d3952d8f61937306ae5/drivers/hid/hid-magicmouse.c#L918)、[HIDP 定义](https://github.com/torvalds/linux/blob/abdf623ddb75b24659018d3952d8f61937306ae5/net/bluetooth/hidp/hidp.h)：仅参考公开协议与生命周期；没有读取或复用 Magic Utilities 实现。
