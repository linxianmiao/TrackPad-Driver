# Magic Trackpad 支持矩阵

本文件区分“代码中已声明”“有真实报告证据”和“通过稳定性门禁”。只有三项都满足时，
设备组合才标记为 `verified`。

## 独立 Magic Trackpad

| 设备 | 连接 | VID:PID | 当前状态 | 说明 |
| --- | --- | --- | --- | --- |
| Magic Trackpad 2（2015，Lightning） | USB | `05ac:0265` | inherited | 保留上游 UMDF Type5 路径，尚未迁移到共享核心 |
| Magic Trackpad 2（2015，Lightning） | Bluetooth | `004c:0265` | experimental | KMDF Filter 已接入共享核心，仍需硬件回归 |
| Magic Trackpad（2021，Lightning 改款） | USB / Bluetooth | `0265` | inherited | 与 2015 款使用相同设备 ID 和报告族 |
| Magic Trackpad（2024，USB-C） | USB | `05ac:0324` | experimental | INF 已绑定，当前暂用 `0265` 参数，缺少真实 fixture |
| Magic Trackpad（2024，USB-C） | Bluetooth | `004c:0324` | bring-up | 当前首要目标：Windows 11 x64 + Bluetooth |
| Magic Trackpad 1（电池款） | Bluetooth | `05ac:030e` | out-of-scope | 不属于当前 MVP |

状态含义：

- `inherited`：沿用上游实现，当前项目尚未重新完成门禁。
- `experimental`：已有绑定或代码路径，但缺少完整硬件证据。
- `bring-up`：正在采集真实设备信息并建立测试基线。
- `verified`：完成协议、功能、电源、重连、HVCI 和卸载回滚门禁。
- `out-of-scope`：当前版本不承诺支持。

## 2024 USB-C Bluetooth 门禁

目标环境：

- Windows 11 x64（原生 AMD64 kernel）
- Apple Bluetooth VID `0x004c`
- Product ID `0x0324`
- HID `Col01` 为触控输入，`Col02` 仅在真实枚举结果确认后绑定 Null Device

按顺序收集并验证：

1. PnP Hardware IDs、Compatible IDs、父子关系、驱动栈和设备接口。
2. 原始 HID capabilities、Report IDs 和实际最大报告长度。
3. 透明 physical READ + VHF virtual PTP 原型；物理 descriptor 不得被替换。
4. 多点触控模式切换前后的 `0x31` 报告。
5. `0x90` 电量报告的长度、百分比和充电状态字段。
6. Bluetooth 关闭/开启、超距恢复和删除后重新配对。
7. 睡眠/唤醒 20 次；重连 20 次；持续输入 2 小时。
8. HVCI、Driver Verifier、安装、升级、卸载和回滚。

在上述项目全部完成前，`0324` 不应宣传为正式支持，也不应提供可安装包。

## 公开协议证据

Linux 主线的 `hid-magicmouse` 已经显式区分 2024 USB-C Trackpad 的
Bluetooth version `0x0314`，并将 USB-C PID 纳入 Trackpad 2 报告族。该实现：

- 将 Bluetooth Input Report ID 定义为 `0x31`；
- 按 4 字节前缀加 `N * 9` 字节触点解析；
- 对 Lightning 和 USB-C 型号的 Bluetooth 连接都发送 `F1 02 01`
  以开启多点模式；
- 在 2024 USB-C Bluetooth 实机上验证电量 `0x90` 为
  `[report-id][status][charge]`。

这些证据支持复用当前 `0x31` 解码方向，但 Windows HID 封装、实际报告
长度、`0x90` 电量格式和断连行为仍必须在目标机器上采集。一手来源：
[Linux `hid-magicmouse.c`](https://github.com/torvalds/linux/blob/abdf623ddb75b24659018d3952d8f61937306ae5/drivers/hid/hid-magicmouse.c#L54-L59)、
[触点帧解析](https://github.com/torvalds/linux/blob/abdf623ddb75b24659018d3952d8f61937306ae5/drivers/hid/hid-magicmouse.c#L397-L413)、
[多点模式开关](https://github.com/torvalds/linux/blob/abdf623ddb75b24659018d3952d8f61937306ae5/drivers/hid/hid-magicmouse.c#L776-L809)、
[电量报告修复与实机验证](https://github.com/torvalds/linux/commit/a1556b48efc157fdda07b52ecc56c7bd1e1786f0)。

## 数据隐私

原始触控 trace 包含坐标，可能泄露用户动作。仓库只接受最小复现片段；长时间 trace、
Bluetooth 地址、Container ID、设备实例后缀和机器名称不得提交。使用
`scripts/windows/Collect-MagicTrackpadDiagnostics.ps1` 时，默认输出会隐藏这些标识。
若同目录存在 `MagicPadHidProbe.exe`，脚本还会采集 descriptor-visible HID caps；探针不读取
或写入 HID report，也不采集序列号。
