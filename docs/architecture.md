# 架构

> 新实现位于 `AmtPtpSource`，采用专用 L2CAP function driver + VHF；见
> [实现及验证边界](native-ptp-source.md)。下文 A/B 为历史 collection 原型候选，
> 当前新工程不沿用其 collection 读路径，也尚未取得实机加载证据。

> 2026-09-07 实机更新：系统驱动下 `004c:0324` 的 Col01 仅声明 8 字节鼠标输入，
> Col02 仅声明 3 字节 `0x90`，没有声明 `0x31`；用户态 Col01 读句柄被拒绝。
> 下文 A/B 的物理 collection 采集前提尚未通过验证，不能直接据此安装原型。
> 实测、可重复检查和底层 transport 候选见 [蓝牙传输检查](bluetooth-transport-findings.md)。

## 生产目标：透明物理 HID + VHF 虚拟 PTP

生产版本不替换 Apple 物理 collection 的 HID descriptor，也不改写其他驱动的派发表。
物理输入和 Windows PTP 输出是两个明确分开的数据面：

```text
Magic Trackpad (Bluetooth, physical Col01)
  └─ HIDClass 上的独立 file object + 连续 IRP_MJ_READ
       └─ AmtPtp source/filter driver
            └─ core/src/amtptp_core.c
                 ├─ decode Apple report 0x31
                 ├─ apply contact lifecycle and policy
                 └─ serialize 50-byte PTP report 0x05
                      └─ VhfReadReportSubmit
                           └─ VHF virtual HID child
                                └─ Windows Precision Touchpad stack
                                     └─ Windows gesture recognition
```

VHF 是微软为“原始 transport/report 不能直接映射为目标 HID”提供的内核框架。源驱动可为
KMDF filter 或 function driver，`vhf.sys` 必须位于源驱动下方。依据：
[Microsoft VHF architecture](https://learn.microsoft.com/en-us/windows-hardware/drivers/hid/virtual-hid-framework--vhf-)。

## Windows 原型 A：同一设备栈

本节仍为候选设计。必须先解决上述原始报告路由问题；独立 VHF child 并不会自动
让物理 HIDClass 接收未声明的 Apple 报告。

首选原型保留 `AmtPtpHidFilter.sys` 作为 `HID\...&Col01` 的透明 source filter，
并使 `vhf.sys` 位于它下方：

1. filter 不注册物理 HID descriptor、Feature 或 READ 的替换处理，只透明转发。
2. 按微软 Firefly 模式查询 `DevicePropertyPhysicalDeviceObjectName`，以独立 file object
   打开 collection，并用一个可取消、single-flight 的 remote `WDFIOTARGET` 连续读取。
3. 仅把合法 `0x31` 帧送入共享核心；`0x90` 电量和未知帧不会触发模式重置。
4. `VhfCreate` / `VhfStart` 创建虚拟 PTP；第一版采用 VHF 默认 buffering，转换完成后
   调用 `VhfReadReportSubmit`。
5. PTP caps、HQA、input mode 和 selective reporting 由 VHF async feature callbacks 完成，
   每个 operation 必须恰好完成一次。

微软没有提供“Bluetooth HID Col01 + Mshidkmdf + source filter + vhf.sys”完全相同的样例，
所以这个拓扑目前只是待验证原型。实际 lower-filter 顺序、remote READ 是否穿透、物理
collection 是否保持不变，必须由 Windows x64 实机证明，不能仅凭 WDK 编译通过作结论。

Windows 10 1903 及以上优先使用声明式 filter levels 保证 source 与 VHF 的顺序；同一
level 内或多个 INF 的 append 顺序不能作为依赖。依据：
[Device filter driver ordering](https://learn.microsoft.com/en-us/windows-hardware/drivers/develop/device-filter-driver-ordering)。

如果原型 A 的栈共存失败，退到原型 B：物理 Col01 只挂透明采集 filter，另建独立
root/software-enumerated VHF source device，两实例通过受控的内核队列传递 PTP 帧。

## 生命周期

- 初始化完整 context、锁和持久 buffer 后，才允许 `VhfStart`；它可能在返回前触发回调。
- 每个物理设备最多一个 lower READ in flight，completion 验证 status、长度和 generation。
- D0 退出或移除时先关闭 producer gate，再停 timer/work item，cancel/purge remote target
  并等待 completion rundown，最后重置共享核心。
- D0 恢复后重新打开 target、切换多点模式并投递一个 READ。
- 最终 cleanup 在 `PASSIVE_LEVEL` 停止所有提交，再调用 `VhfDelete(handle, TRUE)`。

连续输入应使用 `IRP_MJ_READ`，不能轮询 `GET_INPUT_REPORT`；微软明确说明后者可能丢帧，
甚至使部分设备失去响应。依据：
[Opening HID collections](https://learn.microsoft.com/en-us/windows-hardware/drivers/hid/opening-hid-collections)、
[Obtaining HID reports](https://learn.microsoft.com/en-us/windows-hardware/drivers/hid/obtaining-hid-reports)。

## Legacy 路径隔离

当前上游遗留的 `Detour.c` / `include/Hac.h` 会访问另一驱动的私有结构并改写共享
`DRIVER_OBJECT->MajorFunction`。该做法不属于受支持的 Windows filter 合约，可能影响同一
transport driver 管理的其他设备。因此：

- 不得作为 Release 发布产物，也不得进入签名或可安装 CAB（仍允许用 Release 配置做编译验证）；
- 不作为 2024 USB-C Bluetooth 的实现基础；
- 只保留为迁移期间的历史对照，VHF 原型完成后删除。

## 共享转换核心

`core/` 是 simulator 和内核驱动之间的唯一转换实现：

- C17，只用定宽整数和显式 little-endian 读写；
- 不使用浮点、动态内存、CRT 状态或全局可变状态；
- `amtptp_session` 由每个 device context 独立持有；
- Apple 报告最多解析 16 个原始触点，PTP 输出最多 5 个；
- 先保留仍存在的已准入 Contact ID，再准入新 ID；第 6 指不会因输入重排抢占旧 ID；
- 被抑制的 ID 必须先离开 surface，重新出现后才有资格准入；
- Scan Time 为 Apple 毫秒时间戳乘 10，以 100 μs 为单位并按 16 bit 回绕。

## PTP 合约

虚拟 PTP 对 PID `0x0265` 和 `0x0324` 使用同一份 Report Descriptor：

- Report ID `0x05`；
- 5 × 9-byte contact collections；
- `Confidence`、`Tip Switch`、32-bit `Contact ID`；
- 16-bit X/Y、16-bit Scan Time、Contact Count 和 Button；
- 总输入报告固定为 50 字节，不使用打包 struct 或位域；
- Device Certification Status Feature Report 必须完整返回 256-byte 默认 blob。

## 模拟器

`simulator/server.mjs` 在 loopback 地址启动服务，并保持一个
`core/build/amtptp-cli` 子进程：

```text
React controls → encoded Apple 0x31 bytes → native C CLI
                                      ↘ decoded model + PTP bytes → UI
```

预设和 trace seek 会先重置 native session，然后从第 0 帧重放到目标帧，因此 Contact ID
生命周期与顺序播放一致。

## 明确不做

- 不在驱动中实现滚动、缩放或三/四指手势；
- 不模拟鼠标快捷键来代替 Windows PTP；
- 不在 Release 诊断中记录完整原始坐标或 report payload；
- 不依赖 Magic Utilities 的代码、协议实现或资产。
