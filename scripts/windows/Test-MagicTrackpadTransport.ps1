[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$HidProbePath,
    [Parameter(Mandatory = $true)][string]$OutputPath
)

# Check the collection boundary before designing a multitouch producer.
# Opening a handle is the only device operation performed by this script.
# HidP_InitializeReportForID works on preparsed data and local buffers only.
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
if ([IntPtr]::Size -ne 8) { throw 'Run this preflight from x64 PowerShell.' }
$outputFile = [IO.Path]::GetFullPath($OutputPath)
if (Test-Path -LiteralPath $outputFile) { throw 'Output already exists.' }
$probeFile = (Resolve-Path -LiteralPath $HidProbePath).Path
$probeHash = (Get-FileHash -LiteralPath $probeFile -Algorithm SHA256).Hash.ToLowerInvariant()

Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
using Microsoft.Win32.SafeHandles;
namespace MagicPadTransport {
    public sealed class AccessResult {
        public bool Succeeded;
        public int Win32Error;
    }
    public sealed class ParserResult {
        public string Direction, ReportId, Status;
        public int BufferLength;
        public bool Initialized;
    }
    public static class Native {
        [DllImport("kernel32.dll", CharSet=CharSet.Unicode, SetLastError=true)]
        static extern SafeFileHandle CreateFileW(string path, uint access, uint share,
            IntPtr security, uint disposition, uint flags, IntPtr template);
        [DllImport("hid.dll", SetLastError=true)]
        [return:MarshalAs(UnmanagedType.U1)]
        static extern bool HidD_GetPreparsedData(SafeFileHandle handle, out IntPtr data);
        [DllImport("hid.dll")]
        [return:MarshalAs(UnmanagedType.U1)]
        static extern bool HidD_FreePreparsedData(IntPtr data);
        [DllImport("hid.dll")]
        static extern int HidP_InitializeReportForID(int reportType, byte reportId,
            IntPtr data, [Out] byte[] report, uint reportLength);
        public static AccessResult TryOpen(string path, uint access) {
            using (var handle = CreateFileW(path, access, 3, IntPtr.Zero, 3, 0x80, IntPtr.Zero)) {
                return new AccessResult { Succeeded = !handle.IsInvalid,
                    Win32Error = handle.IsInvalid ? Marshal.GetLastWin32Error() : 0 };
            }
        }
        public static ParserResult CheckReport(string path, int type, byte id, int length) {
            var result = new ParserResult { Direction = type == 0 ? "input" : "feature",
                ReportId = "0x" + id.ToString("x2"), BufferLength = length };
            if (length == 0) { result.Status = "noBufferDeclared"; return result; }
            if (length < 0 || length > 65535) throw new ArgumentOutOfRangeException("length");
            using (var handle = CreateFileW(path, 0, 3, IntPtr.Zero, 3, 0x80, IntPtr.Zero)) {
                if (handle.IsInvalid) throw new System.ComponentModel.Win32Exception(Marshal.GetLastWin32Error());
                IntPtr data;
                if (!HidD_GetPreparsedData(handle, out data))
                    throw new System.ComponentModel.Win32Exception(Marshal.GetLastWin32Error());
                try {
                    var bytes = new byte[length];
                    int status = HidP_InitializeReportForID(type, id, data, bytes, (uint)length);
                    result.Status = "0x" + unchecked((uint)status).ToString("x8");
                    result.Initialized = status == 0x00110000;
                    return result;
                } finally { HidD_FreePreparsedData(data); }
            }
        }
    }
}
'@

$startInfo = [Diagnostics.ProcessStartInfo]::new()
$startInfo.FileName = $probeFile
$startInfo.UseShellExecute = $false
$startInfo.CreateNoWindow = $true
$startInfo.RedirectStandardOutput = $true
$startInfo.RedirectStandardError = $true
$process = [Diagnostics.Process]::Start($startInfo)
try {
    $stdout = $process.StandardOutput.ReadToEndAsync()
    $stderr = $process.StandardError.ReadToEndAsync()
    if (-not $process.WaitForExit(15000)) {
        $process.Kill()
        $process.WaitForExit()
        throw 'HID probe exceeded 15 seconds.'
    }
    if ($process.ExitCode -ne 0) { throw ('HID probe failed with exit code ' + $process.ExitCode) }
    $probe = $stdout.GetAwaiter().GetResult() | ConvertFrom-Json
} finally { $process.Dispose() }
if ($probe.schema -ne 'magicpad-hid-probe/v1') { throw 'Unexpected HID probe schema.' }

$collections = @()
foreach ($device in @($probe.devices)) {
    # Scope access tests to physical Bluetooth 004c:0324 only, never USB or a
    # vendor-rewritten collection. Do not copy paths into the output document.
    if (-not $device.attributes.available -or
        $device.attributes.vendorId -ine '0x004c' -or
        $device.attributes.productId -ine '0x0324' -or
        -not $device.match.exactPhysicalPnpId) { continue }
    $path = [string]$device.interfacePath
    if ($path -notmatch '(?i)^\\\\\?\\hid#.*vid&0001004c_pid&0324&col[0-9a-f]{2}#') {
        throw 'Probe returned an unexpected physical Bluetooth interface path.'
    }
    if (-not $device.descriptorCapsComplete) { continue }
    $inputIds = @()
    $featureIds = @()
    foreach ($kind in 'buttonCaps', 'valueCaps') {
        $inputIds += @($device.descriptorVisibleCaps.$kind.input.caps | ForEach-Object { [int]$_.reportId })
        $featureIds += @($device.descriptorVisibleCaps.$kind.feature.caps | ForEach-Object { [int]$_.reportId })
    }
    $parserChecks = @()
    foreach ($id in @(@($inputIds) + @(0x31) | Sort-Object -Unique)) {
        $parserChecks += [MagicPadTransport.Native]::CheckReport($path, 0, $id, $device.caps.inputReportByteLength)
    }
    foreach ($id in @(@($featureIds) + @(0xf1) | Sort-Object -Unique)) {
        $parserChecks += [MagicPadTransport.Native]::CheckReport($path, 2, $id, $device.caps.featureReportByteLength)
    }
    $collections += [ordered]@{
        collection = $device.collection
        usagePage = $device.caps.usagePage
        usage = $device.caps.usage
        inputReportBytes = $device.caps.inputReportByteLength
        featureReportBytes = $device.caps.featureReportByteLength
        inputReportIds = @($inputIds | Sort-Object -Unique)
        featureReportIds = @($featureIds | Sort-Object -Unique)
        access = [ordered]@{
            metadata = [MagicPadTransport.Native]::TryOpen($path, 0)
            read = [MagicPadTransport.Native]::TryOpen($path, 2147483648)
            write = [MagicPadTransport.Native]::TryOpen($path, 1073741824)
        }
        parserChecks = $parserChecks
    }
}
$document = [ordered]@{
    schema = 'magicpad-transport-preflight/v1'
    collectedAtUtc = (Get-Date).ToUniversalTime().ToString('o')
    processArchitecture = $env:PROCESSOR_ARCHITECTURE
    probeStatus = $probe.status
    probeBinarySha256 = $probeHash
    scope = 'userModePhysicalBluetoothHidCollection'
    reportIoAttempted = $false
    modeSwitchAttempted = $false
    rawCoordinatesCollected = $false
    collections = $collections
}
$document | ConvertTo-Json -Depth 12 | Set-Content -LiteralPath $outputFile -Encoding UTF8
Write-Host "Transport preflight saved to $outputFile"
