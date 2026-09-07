[CmdletBinding()]
param()
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
if ([IntPtr]::Size -ne 8) { throw 'Use x64 PowerShell.' }
# Reads only the new source driver's fixed-size status IOCTL. It cannot send
# feature reports, switch device mode, install drivers, or retrieve coordinates.
if (-not ('MagicPadSourceStatus.Reader' -as [type])) {
    $sourcePath = Join-Path $PSScriptRoot 'SourceStatus.cs'
    if (-not (Test-Path -LiteralPath $sourcePath)) {
        $sourcePath = Join-Path $PSScriptRoot '../../tools/windows/MagicPadSourceStatus/SourceStatus.cs'
    }
    Add-Type -Path $sourcePath
}
$devices = @([MagicPadSourceStatus.Reader]::Read())
[ordered]@{
    schema = 'magicpad-source-status/v5'
    status = $(if ($devices.Count -eq 0) { 'sourceNotPresent' } else { 'ok' })
    devices = $devices
} | ConvertTo-Json -Depth 4
