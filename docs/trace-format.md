# `magicpad-trace/v1`

模拟器导入/导出的 JSON trace 用于复现转换问题，不是 Windows ETL 文件。

```json
{
  "schema": "magicpad-trace/v1",
  "source": "synthetic",
  "device": {
    "vendorId": "0x004c",
    "productId": "0x0324",
    "transport": "synthetic"
  },
  "frames": [
    {
      "captureTimestampUs": 0,
      "rawReportHex": "31000000",
      "ptpReportHex": "0500000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000"
    }
  ]
}
```

字段：

- `schema`：固定为 `magicpad-trace/v1`
- `source`：`captured` 或 `synthetic`
- `device.transport`：`bluetooth`、`usb` 或 `synthetic`
- `captureTimestampUs`：相对 capture 起点的微秒时间
- `rawReportHex`：包含 Report ID 的完整 Apple HID 报告
- `ptpReportHex`：可选的 50 字节黄金输出

导入器只信任 `rawReportHex` 并重新运行当前 native C 核心；`ptpReportHex` 用于差异比较，
不会直接显示为当前输出。

真实设备 trace 可能包含可推断用户动作的坐标数据。提交 issue 前应只保留最小复现帧，
不要上传长时间原始采集。
