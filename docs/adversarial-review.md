# 对抗性方案审查与架构更正

## 前两轮结论

实现前两轮审查主要检查 PTP descriptor/feature/input 是否自洽、bitfield 与 packing、
Scan Time、Contact ID 生命周期、第 6 指、休眠重连、HVCI、GPL 边界，以及 simulator
与驱动是否共用同一转换实现。

当时根据现有代码能够拦截 HID IOCTL 并返回替换 descriptor，暂时保留了上游
descriptor-replacement lower filter。这只能证明代码路径存在，不能证明它使用了受支持的
Windows 内核扩展点。

## 第三轮：内核接口级复审

对 `Detour.c` 和 `include/Hac.h` 的逐项复审发现发布阻断问题：

- 通过私有结构解释另一个驱动的 `DriverExtension`；
- 获取 lower device 后，改写其共享 `DRIVER_OBJECT->MajorFunction` 表；
- 没有可靠的 per-device 作用域、恢复顺序和并发协议；
- 同一 transport driver 下的非目标设备也可能被影响。

即使把指针交换改成原子操作并在卸载时恢复，也仍然是在依赖另一个驱动的私有布局和全局
派发表，不会因此成为受支持的 filter 设计。因此更正此前结论：生产版本必须迁移到 VHF，
现有 detour 不得进入签名或可安装包。

微软 VHF 文档明确允许 source driver 是 KMDF filter/function，并要求 `vhf.sys` 位于其
下方；物理 HID 连续输入应使用已打开 collection 上的 `IRP_MJ_READ`。证据：

- [Virtual HID Framework](https://learn.microsoft.com/en-us/windows-hardware/drivers/hid/virtual-hid-framework--vhf-)
- [Opening HID collections](https://learn.microsoft.com/en-us/windows-hardware/drivers/hid/opening-hid-collections)
- [Obtaining HID reports](https://learn.microsoft.com/en-us/windows-hardware/drivers/hid/obtaining-hid-reports)
- [Microsoft Firefly filter sample](https://github.com/microsoft/Windows-driver-samples/tree/main/hid/firefly)

## 当前结论

项目仍是“有条件推进”，但条件已收紧：

1. 保留已经通过可移植测试的共享转换核心和 simulator。
2. 先用只读 PnP/HID caps 工具建立真实 `004c:0324` Bluetooth 基线。
3. 完成透明 physical READ + VHF virtual PTP 原型，并删除 detour/private layout 依赖。
4. 在原生 Windows 11 x64 上验证 filter 顺序、功能、睡眠/重连、HVCI、Driver Verifier、
   卸载和回滚。
5. 所有门禁通过前，只能称为 bring-up 源码，不能提供可安装驱动或宣称正式支持。
