# 架构

## 数据路径

```text
Magic Trackpad (Bluetooth HID)
  └─ Apple multitouch input report 0x31
       └─ AmtPtpHidFilter.sys (KMDF lower filter)
            ├─ descriptor replacement / PTP feature reports
            └─ core/src/amtptp_core.c
                 ├─ decode Apple bytes
                 ├─ apply contact lifecycle and policy
                 └─ serialize 50-byte PTP report 0x05
                      └─ Windows HIDClass / Precision Touchpad stack
                           └─ Windows gesture recognition
```

USB 设备继续使用上游的 UMDF transport。Bluetooth `Col01` 绑定 KMDF lower filter，
`Col02` 绑定 Null Device，硬件 ID 由 `build/AmtPtpDevice_*.inf` 声明。

## 共享转换核心

`core/` 是 simulator 和内核驱动之间的唯一转换实现：

- C17，只用定宽整数和显式 little-endian 读写
- 不使用浮点、动态内存、CRT 状态或全局可变状态
- `amtptp_session` 由每个 WDF device context 独立持有
- Apple 报告最多解析 16 个原始触点，PTP 输出最多 5 个
- 先保留仍存在的已准入 Contact ID，再准入新 ID；第 6 指不会因输入重排抢占旧 ID
- 被抑制的 ID 必须先离开 surface，重新出现后才有资格准入
- Scan Time 为 Apple 毫秒时间戳乘 10，以 100 μs 为单位并按 16 bit 回绕

## PTP 合约

驱动对 PID `0x0265` 和 `0x0324` 返回相同的 PTP Report Descriptor：

- Report ID `0x05`
- 5 × 9-byte contact collections
- `Confidence`、`Tip Switch`、32-bit `Contact ID`
- 16-bit X/Y
- 16-bit Scan Time
- Contact Count 和 Button

总输入报告长度固定为 50 字节。序列化不使用打包 struct 或位域。

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

- 不在驱动中实现滚动、缩放或三/四指手势
- 不模拟鼠标快捷键来代替 Windows PTP
- 不在 Release 诊断中记录完整原始坐标或 report payload
- 不依赖 Magic Utilities 的代码、协议实现或资产
