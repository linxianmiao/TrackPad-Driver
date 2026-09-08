# Windows 蓝牙列表电量

`AmtPtpSource` 0.2.0.6 将 Magic Trackpad 的真实百分比直接提供给 Windows 的
“设置 → 蓝牙和设备”列表。无需单独弹窗、托盘程序、登录启动项或 Magic Utilities。
首次连接可能需要等待下一次查询（约一分钟），设置页可能稍后刷新。

## 读取和有效性

- 驱动每 60 秒通过 HIDP GET_REPORT Input 命令 `41 90` 请求电量；control 通道使用
  独立异步 WDF 读写请求、BRB 和缓冲，先挂起接收再发送请求。触点在 interrupt
  通道继续读取，首次无应答也会在下个周期继续查询。
- 只接受完整四字节 HIDP 帧 `A1 90 status percent`，百分比来自第四字节，范围
  0–100。状态字节不会误读为电量，目前也不解释为“正在充电”。
- 控制请求超时或非法响应不拆掉正常触控连接；停止/移除先取消并等待电量请求完成，
  再关闭通道。查询有超时和应答数量上限。
- 断连、暂停或超过 180 秒无新读数时清除电量属性；未知值不冒充 0%。真正的 0%
  是有效读数。时间使用包含挂起时间的系统计时，恢复时不会把旧样本当作新读数。

## Windows 集成

驱动在自己的 Bluetooth HID 服务设备节点上使用 `WdfDeviceAssignProperty` 写入
`{104EA319-6EE2-4701-BD47-8DDBF425BBE5}, 2`，类型 `DEVPROP_TYPE_BYTE`。
本机 Windows 11 已验证会将该属性汇入物理设备的蓝牙卡片；该键未出现在本次 SDK 的
公开 `devpkey.h` 中，因此这是已验证的 Windows 实现行为，不保证所有 Windows 版本。
它与 Shell 的 `System.Devices.BatteryLife` 键 `{49CD1F76-5626-4B17-A4E8-18B4AA1A2213}, 10`
不同，不能混用 GUID。

按 WDF 文档设置 `PLUGPLAY_PROPERTY_PERSISTENT`，驱动初始化、读数过期和正常
断开/暂停时显式删除属性。异常掉电不能保证执行清理；下次加载先清除旧值。
写属性在被动级、释放自旋锁后执行，百分比相同且上次成功时跳过重复写入。
失败每分钟重试，不停止手势，不写入其他蓝牙设备或电脑主电池的信息。

诊断命令：

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\scripts\windows\Read-MagicPadSourceStatus.ps1
```

状态 v5 为 148 字节，包含实际读数和 `BatteryPropertyStatus`（0 表示成功）、
`BatteryPropertyPercent`（-1 表示已清除或尚未发布）、`BatteryPropertyUpdates`。
工具兼容 v1 52、v2 88、v3 116、v4 136 字节；旧驱动缺少的字段不能解读成发布成功。
`SourceStatus.cs` 随只读诊断工具分发，不返回坐标、地址或序列号。

## 验证

核心测试覆盖状态与电量偏移、全部 0–255 数值边界、短包/超长包、空指针和错误
HIDP/report ID；既有测试验证电量包不会打断触控会话。Windows 构建使用 `/W4 /WX`、
代码分析、Universal DDI、InfVerif、签名和 catalog 成员检查。

2026-09-08 本机的 `004c:0324` 返回 `A1 90 04 64`，实际电量 100%。已观察多轮
主动查询成功，读/写状态均为 0，报告计数递增且读数年龄重置。用户确认触控正常，
并确认 Windows 蓝牙列表显示 100%。首次查询偶有超时，下次查询可恢复。
更新到 0.2.0.6 后再次验证：PnP 状态 OK，原生电量属性为 Byte 100，
`BatteryPropertyStatus=0`；已接收 1,779 个有效触点报告，主动查询也再次成功返回
四字节电量。驱动初始化删除了探测时写入的属性，随后由真实报告重新发布。
充电状态变化、实际电量下降、断连/睡眠恢复和长期运行仍需单独实测。

参考：[Linux 对同款 USB-C Bluetooth 设备的电量修复](https://github.com/torvalds/linux/commit/a1556b48efc157fdda07b52ecc56c7bd1e1786f0)、
[Linux HIDP](https://github.com/torvalds/linux/blob/abdf623ddb75b24659018d3952d8f61937306ae5/net/bluetooth/hidp/core.c)、
[WdfDeviceAssignProperty](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/wdfdevice/nf-wdfdevice-wdfdeviceassignproperty)、
[WDF_DEVICE_PROPERTY_DATA](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/wdfdevice/ns-wdfdevice-_wdf_device_property_data)、
[System.Devices.BatteryLife](https://learn.microsoft.com/en-us/windows/win32/properties/props-system-devices-batterylife)。
