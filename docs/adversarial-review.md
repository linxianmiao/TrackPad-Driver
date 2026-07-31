# 两轮对抗性方案审查

实现前使用 Claude CLI（Opus / high）进行了两轮独立审查。

## 第一轮：红队质疑

重点攻击：

- 是否错误地把 VHF 当成现有 descriptor replacement 的必需条件
- PTP Report Descriptor、Feature Report 与 50-byte Input Report 是否自洽
- bitfield / packing、Scan Time、Contact ID 生命周期和第 6 指行为
- 休眠、取消、Bluetooth 重连、测试签名与 HVCI
- GPL 派生、Magic Utilities 专有边界和发布证书
- macOS simulator 是否可能与真实驱动出现双实现漂移

该轮提出的“必须改成 VHF”和“测试签名必然与 HVCI 不兼容”被标记为待证伪假设，
没有直接纳入实现。

## 第二轮：证据复核

第二轮以固定上游源码为证据重新检查：

- `Queue.c` 已拦截 `IOCTL_HID_GET_REPORT_DESCRIPTOR` / `SET_FEATURE`
- `Hid.c` 已向 `0x0265` / `0x0324` 返回 PTP 描述符和 Feature Reports
- INF 将 Bluetooth `Col01` 绑定 lower filter、`Col02` 绑定 Null Device
- 因此 MVP 保留 descriptor-replacement KMDF lower filter，不引入 VHF
- HVCI 是否接受测试包是硬件门禁，不在没有 Windows 机器时作结论
- simulator 直接调用同一 native C core，不另写 WASM/TypeScript 转换器

## 结论

结论是“有条件推进”：

- 架构可行；
- 共享核心、显式序列化、稳定五指生命周期和测试签名隔离必须先完成；
- Windows x64 编译、真实 PID `0x0324` Bluetooth、睡眠/重连、HVCI 和卸载回滚仍是
  发布阻断门禁；
- 未通过这些门禁前只能称为开发预览，不能称为生产可用驱动。
