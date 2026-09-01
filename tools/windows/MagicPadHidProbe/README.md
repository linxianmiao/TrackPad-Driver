# MagicPadHidProbe

`MagicPadHidProbe` is a metadata-only Windows x64 probe for the Bluetooth
Magic Trackpad USB-C (`VID 004c`, `PID 0324`). It enumerates HID interfaces
with SetupAPI, opens only target candidates with `CreateFile` desired access
set to zero, and exports the descriptor-visible `HidP` capabilities as JSON.

It never reads, gets, sets, writes, or flushes HID reports, and it never asks
the device for manufacturer, product, or serial strings. Document status is
`ok` only when every target collection and descriptor-capability query succeeds;
otherwise it reports `partial`, `capsUnavailable`, or `targetNotFound`.

When run directly, the standalone JSON includes the HID `interfacePath` and
PnP `instanceId`; treat that output as potentially identifying. For diagnostic
bundles, use `Collect-MagicTrackpadDiagnostics.ps1`, which redacts those fields
by default. Pass `-IncludeSensitiveIdentifiers` only when the unredacted IDs
are explicitly needed.

Build from a Visual Studio 2022 developer prompt:

```powershell
msbuild .\tools\windows\MagicPadHidProbe\MagicPadHidProbe.vcxproj `
  -p:Configuration=Release -p:Platform=x64
```

The executable is written to
`tools\windows\MagicPadHidProbe\build\x64\Release\MagicPadHidProbe.exe`.
