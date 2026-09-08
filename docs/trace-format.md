# `magicpad-trace/v1`

用于保存最小复现帧和转换期望值的 JSON trace，不是 Windows ETL 文件。
本地 Web 模拟器已移除；此格式继续保留用于 fixture 与命令行复现，不提供浏览器导入/导出。

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

`core/build/amtptp-cli` 接受 JSON-lines 请求，而不是整份 trace。复现时先发送
`{"command":"reset"}`，再按 `frames` 顺序将每个 `rawReportHex` 作为请求的 `reportHex`
字段提交。CLI 使用当前 native C 核心重新计算输出，调用方需要显式将返回的
`ptpReportHex` 与 fixture 中的可选期望值比较；CLI 不会自动完成 trace 导入或黄金值断言。
不要用期望值代替实际输出，也不要因重放而把 `captured` 来源改成 `synthetic`。

真实设备 trace 可能包含可推断用户动作的坐标数据。提交 issue 前应只保留最小复现帧，
不要上传长时间原始采集。
