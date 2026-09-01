[CmdletBinding()]
param(
    [string]$OutputDirectory,

    [ValidateRange(0, 300)]
    [int]$TraceSeconds = 0,

    [string]$WprProfilePath,

    [switch]$IncludeSensitiveIdentifiers,

    [switch]$NoArchive
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"
$ProgressPreference = "SilentlyContinue"
$utf8NoBom = New-Object System.Text.UTF8Encoding($false)
$targetPattern = '(?i)VID&0001004C_PID&0324'
$captureStartedAt = Get-Date

function Write-Utf8File {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Path,

        [AllowEmptyString()]
        [string]$Content
    )

    [System.IO.File]::WriteAllText($Path, $Content, $script:utf8NoBom)
}

function Append-Utf8File {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Path,

        [AllowEmptyString()]
        [string]$Content
    )

    [System.IO.File]::AppendAllText(
        $Path,
        $Content + [Environment]::NewLine,
        $script:utf8NoBom)
}

function ConvertTo-JsonText {
    param(
        [AllowNull()]
        [object]$InputObject,

        [ValidateRange(1, 100)]
        [int]$Depth = 8
    )

    return ConvertTo-Json -InputObject $InputObject -Depth $Depth
}

function Protect-DiagnosticText {
    param(
        [AllowEmptyString()]
        [string]$Text
    )

    if ($IncludeSensitiveIdentifiers -or [string]::IsNullOrEmpty($Text)) {
        return $Text
    }

    $protected = $Text
    if (-not [string]::IsNullOrEmpty($env:COMPUTERNAME)) {
        $protected = $protected.Replace($env:COMPUTERNAME, "COMPUTER_REDACTED")
    }
    if (-not [string]::IsNullOrEmpty($env:USERNAME)) {
        $protected = $protected.Replace($env:USERNAME, "USER_REDACTED")
    }

    $protected = [regex]::Replace(
        $protected,
        '(?im)(HID|BTHENUM|BTHLEDEVICE|USB|PCI)\\([^\r\n\\]+)\\[^\r\n\s"]+',
        '$1\$2\INSTANCE_REDACTED')
    $protected = [regex]::Replace(
        $protected,
        '(?im)^(\s*Container ID\s*:\s*).+$',
        '$1{CONTAINER_REDACTED}')
    $protected = [regex]::Replace(
        $protected,
        '(?i)(DEV_|BluetoothDevice_)[0-9A-F]{12}',
        '$1ADDRESS_REDACTED')
    $protected = [regex]::Replace(
        $protected,
        '(?i)(?<![0-9A-F])(?:[0-9A-F]{2}[:-]){5}[0-9A-F]{2}(?![0-9A-F])',
        'ADDRESS_REDACTED')
    $protected = [regex]::Replace(
        $protected,
        '(?i)\{[0-9A-F]{8}-[0-9A-F]{4}-[0-9A-F]{4}-[0-9A-F]{4}-[0-9A-F]{12}\}',
        '{GUID_REDACTED}')

    return $protected
}

function Convert-PropertyValue {
    param(
        [AllowNull()]
        [object]$Value
    )

    if ($null -eq $Value) {
        return $null
    }
    if ($Value -is [System.Array]) {
        return @($Value | ForEach-Object { Protect-DiagnosticText ([string]$_) })
    }
    return Protect-DiagnosticText ([string]$Value)
}

function Invoke-TextCommand {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Command,

        [Parameter(Mandatory = $true)]
        [string[]]$Arguments
    )

    $output = & $Command @Arguments 2>&1 | Out-String
    return Protect-DiagnosticText $output
}

function Get-SelectedDeviceProperty {
    param(
        [Parameter(Mandatory = $true)]
        [string]$InstanceId,

        [Parameter(Mandatory = $true)]
        [string[]]$KeyNames
    )

    $properties = [ordered]@{}
    foreach ($keyName in $KeyNames) {
        try {
            $property = Get-PnpDeviceProperty -InstanceId $InstanceId `
                -KeyName $keyName -ErrorAction Stop
            if (-not $IncludeSensitiveIdentifiers -and
                $keyName -eq "DEVPKEY_Device_ContainerId") {
                $properties[$keyName] = "CONTAINER_REDACTED"
            }
            else {
                $properties[$keyName] = Convert-PropertyValue $property.Data
            }
        }
        catch {
            $properties[$keyName] = $null
        }
    }
    return $properties
}

if ([string]::IsNullOrWhiteSpace($OutputDirectory)) {
    $directoryName = "magicpad-diagnostics-{0}" -f (Get-Date -Format "yyyyMMdd-HHmmss")
    $OutputDirectory = Join-Path (Get-Location) $directoryName
}
$OutputDirectory = [System.IO.Path]::GetFullPath($OutputDirectory)

if (Test-Path -LiteralPath $OutputDirectory) {
    throw "Output directory already exists: $OutputDirectory"
}

New-Item -ItemType Directory -Path $OutputDirectory | Out-Null
Write-Host "Collecting read-only diagnostics in $OutputDirectory"

$systemInfo = [ordered]@{
    collectedAtUtc = (Get-Date).ToUniversalTime().ToString("o")
    windows = $null
    deviceGuard = $null
    secureBoot = "unknown"
}

try {
    $operatingSystem = Get-CimInstance -ClassName Win32_OperatingSystem
    $computerSystem = Get-CimInstance -ClassName Win32_ComputerSystem
    $systemInfo.windows = [ordered]@{
        caption = $operatingSystem.Caption
        version = $operatingSystem.Version
        buildNumber = $operatingSystem.BuildNumber
        osArchitecture = $operatingSystem.OSArchitecture
        processArchitecture = $env:PROCESSOR_ARCHITECTURE
        systemType = $computerSystem.SystemType
        manufacturer = $computerSystem.Manufacturer
        model = $computerSystem.Model
        hypervisorPresent = $computerSystem.HypervisorPresent
    }
}
catch {
    $systemInfo.windows = @{ error = $_.Exception.Message }
}

try {
    $deviceGuard = Get-CimInstance -Namespace "root\Microsoft\Windows\DeviceGuard" `
        -ClassName Win32_DeviceGuard
    $systemInfo.deviceGuard = [ordered]@{
        virtualizationBasedSecurityStatus = $deviceGuard.VirtualizationBasedSecurityStatus
        securityServicesConfigured = @($deviceGuard.SecurityServicesConfigured)
        securityServicesRunning = @($deviceGuard.SecurityServicesRunning)
    }
}
catch {
    $systemInfo.deviceGuard = @{ error = $_.Exception.Message }
}

try {
    $systemInfo.secureBoot = [string](Confirm-SecureBootUEFI)
}
catch {
    $systemInfo.secureBoot = "unavailable: $($_.Exception.Message)"
}

Write-Utf8File -Path (Join-Path $OutputDirectory "system.json") `
    -Content (Protect-DiagnosticText (ConvertTo-JsonText $systemInfo -Depth 8))

if ($env:PROCESSOR_ARCHITECTURE -ne "AMD64") {
    Write-Warning "Expected an x64 Windows process, found $env:PROCESSOR_ARCHITECTURE."
}

try {
    $powerCapabilities = Invoke-TextCommand -Command "powercfg.exe" -Arguments @("/a")
    Write-Utf8File -Path (Join-Path $OutputDirectory "power-capabilities.txt") `
        -Content $powerCapabilities
}
catch {
    Write-Utf8File -Path (Join-Path $OutputDirectory "power-capabilities-error.txt") `
        -Content (Protect-DiagnosticText $_.Exception.ToString())
}

$propertyKeys = @(
    "DEVPKEY_Device_DeviceDesc",
    "DEVPKEY_Device_BusReportedDeviceDesc",
    "DEVPKEY_Device_HardwareIds",
    "DEVPKEY_Device_CompatibleIds",
    "DEVPKEY_Device_Class",
    "DEVPKEY_Device_ClassGuid",
    "DEVPKEY_Device_Service",
    "DEVPKEY_Device_Driver",
    "DEVPKEY_Device_DriverVersion",
    "DEVPKEY_Device_DriverDate",
    "DEVPKEY_Device_Manufacturer",
    "DEVPKEY_Device_EnumeratorName",
    "DEVPKEY_Device_Parent",
    "DEVPKEY_Device_Children",
    "DEVPKEY_Device_ContainerId",
    "DEVPKEY_Device_ProblemCode",
    "DEVPKEY_Device_ProblemStatus"
)

$allDevices = @(Get-PnpDevice -ErrorAction Stop)
$targetDevices = @($allDevices | Where-Object {
    $_.InstanceId -match $targetPattern
})

try {
    $bluetoothAdapters = @($allDevices | Where-Object {
        $_.Class -eq "Bluetooth" -and
        ($_.FriendlyName -match '(?i)(adapter|radio|bluetooth)' -or
         $_.InstanceId -match '(?i)^(USB|PCI)\\')
    } | ForEach-Object {
        [ordered]@{
            status = [string]$_.Status
            friendlyName = Protect-DiagnosticText ([string]$_.FriendlyName)
            instanceId = if ($IncludeSensitiveIdentifiers) {
                [string]$_.InstanceId
            } else {
                Protect-DiagnosticText ([string]$_.InstanceId)
            }
            properties = Get-SelectedDeviceProperty -InstanceId $_.InstanceId `
                -KeyNames @(
                    "DEVPKEY_Device_HardwareIds",
                    "DEVPKEY_Device_Service",
                    "DEVPKEY_Device_Driver",
                    "DEVPKEY_Device_DriverVersion",
                    "DEVPKEY_Device_DriverDate",
                    "DEVPKEY_Device_Manufacturer",
                    "DEVPKEY_Device_ProblemCode"
                )
        }
    })
    Write-Utf8File -Path (Join-Path $OutputDirectory "bluetooth-adapters.json") `
        -Content (Protect-DiagnosticText (ConvertTo-JsonText $bluetoothAdapters -Depth 10))
}
catch {
    Write-Utf8File -Path (Join-Path $OutputDirectory "bluetooth-adapters-error.txt") `
        -Content (Protect-DiagnosticText $_.Exception.ToString())
}

$deviceRecords = @()
$deviceIndex = 0
foreach ($device in $targetDevices) {
    $deviceIndex++
    $alias = "device-{0:d2}" -f $deviceIndex
    $properties = Get-SelectedDeviceProperty -InstanceId $device.InstanceId `
        -KeyNames $propertyKeys

    $deviceRecords += [ordered]@{
        alias = $alias
        status = [string]$device.Status
        class = [string]$device.Class
        friendlyName = Protect-DiagnosticText ([string]$device.FriendlyName)
        instanceId = if ($IncludeSensitiveIdentifiers) {
            [string]$device.InstanceId
        } else {
            $alias
        }
        properties = $properties
    }

    try {
        $pnpOutput = Invoke-TextCommand -Command "pnputil.exe" -Arguments @(
            "/enum-devices",
            "/instanceid", [string]$device.InstanceId,
            "/deviceids",
            "/relations",
            "/services",
            "/stack",
            "/drivers",
            "/interfaces",
            "/properties"
        )
        Write-Utf8File -Path (Join-Path $OutputDirectory "$alias-pnputil.txt") `
            -Content $pnpOutput
    }
    catch {
        Write-Utf8File -Path (Join-Path $OutputDirectory "$alias-pnputil-error.txt") `
            -Content (Protect-DiagnosticText $_.Exception.ToString())
    }
}

Write-Utf8File -Path (Join-Path $OutputDirectory "devices.json") `
    -Content (ConvertTo-JsonText $deviceRecords -Depth 10)

try {
    $driverInventory = Invoke-TextCommand -Command "pnputil.exe" `
        -Arguments @("/enum-drivers", "/class", "HIDClass")
    Write-Utf8File -Path (Join-Path $OutputDirectory "hid-driver-inventory.txt") `
        -Content $driverInventory
}
catch {
    Write-Utf8File -Path (Join-Path $OutputDirectory "hid-driver-inventory-error.txt") `
        -Content (Protect-DiagnosticText $_.Exception.ToString())
}

try {
    $driverServices = @(Get-CimInstance -ClassName Win32_SystemDriver | Where-Object {
        $_.Name -match '(?i)(AmtPtp|MagicTrackpad|WUDFRd|HidBth)'
    } | Select-Object Name, DisplayName, State, StartMode, PathName, ServiceType)
    Write-Utf8File -Path (Join-Path $OutputDirectory "driver-services.json") `
        -Content (Protect-DiagnosticText (ConvertTo-JsonText $driverServices -Depth 6))
}
catch {
    Write-Utf8File -Path (Join-Path $OutputDirectory "driver-services-error.txt") `
        -Content (Protect-DiagnosticText $_.Exception.ToString())
}

$eventStart = (Get-Date).AddHours(-2)
try {
    $systemEvents = @(Get-WinEvent -FilterHashtable @{
        LogName = "System"
        StartTime = $eventStart
    } -ErrorAction Stop | Where-Object {
        $_.ProviderName -match '(?i)(Kernel-PnP|UserPnp|DriverFrameworks|BTHUSB|BTHMINI|HidBth|Kernel-Power|Power-Troubleshooter)' -or
        $_.Message -match '(?i)(0324|Magic Trackpad|AmtPtp)'
    } | Select-Object -First 500 TimeCreated, Id, LevelDisplayName, ProviderName, Message)
    Write-Utf8File -Path (Join-Path $OutputDirectory "system-events.json") `
        -Content (Protect-DiagnosticText (ConvertTo-JsonText $systemEvents -Depth 6))
}
catch {
    Write-Utf8File -Path (Join-Path $OutputDirectory "system-events-error.txt") `
        -Content (Protect-DiagnosticText $_.Exception.ToString())
}

try {
    $codeIntegrityEvents = @(Get-WinEvent -FilterHashtable @{
        LogName = "Microsoft-Windows-CodeIntegrity/Operational"
        StartTime = $eventStart
    } -ErrorAction Stop | Where-Object {
        $_.Message -match '(?i)(AmtPtp|MagicTrackpad)'
    } | Select-Object -First 200 TimeCreated, Id, LevelDisplayName, ProviderName, Message)
    Write-Utf8File -Path (Join-Path $OutputDirectory "code-integrity-events.json") `
        -Content (Protect-DiagnosticText (ConvertTo-JsonText $codeIntegrityEvents -Depth 6))
}
catch {
    Write-Utf8File -Path (Join-Path $OutputDirectory "code-integrity-events-error.txt") `
        -Content (Protect-DiagnosticText $_.Exception.ToString())
}

$setupApiLog = Join-Path $env:windir "inf\setupapi.dev.log"
if (Test-Path -LiteralPath $setupApiLog) {
    try {
        $setupMatches = @(Select-String -Path $setupApiLog `
            -Pattern '0324|MagicTrackpad|AmtPtp' -AllMatches | Select-Object -Last 500)
        Write-Utf8File -Path (Join-Path $OutputDirectory "setupapi-matches.txt") `
            -Content (Protect-DiagnosticText ($setupMatches | ForEach-Object {
                "{0}:{1}: {2}" -f $_.Path, $_.LineNumber, $_.Line
            } | Out-String))
    }
    catch {
        Write-Utf8File -Path (Join-Path $OutputDirectory "setupapi-error.txt") `
            -Content (Protect-DiagnosticText $_.Exception.ToString())
    }
}

if ($TraceSeconds -gt 0) {
    if ([string]::IsNullOrWhiteSpace($WprProfilePath)) {
        $WprProfilePath = Join-Path $PSScriptRoot "..\..\AmtPtpHidFilter\AmtPtpHidFilter.wprp"
    }
    $WprProfilePath = [System.IO.Path]::GetFullPath($WprProfilePath)
    $etlPath = Join-Path $OutputDirectory "magicpad-filter.etl"
    $wprLogPath = Join-Path $OutputDirectory "wpr.txt"
    $wprStarted = $false

    try {
        if (-not (Test-Path -LiteralPath $WprProfilePath)) {
            throw "WPR profile not found: $WprProfilePath"
        }

        $startOutput = & wpr.exe -start "$WprProfilePath!AmtPtpHidFilter" `
            -filemode 2>&1 | Out-String
        $startExitCode = $LASTEXITCODE
        Write-Utf8File -Path $wprLogPath -Content (Protect-DiagnosticText $startOutput)
        if ($startExitCode -ne 0) {
            throw "wpr -start failed with exit code $startExitCode"
        }

        $wprStarted = $true
        Write-Host "Move one finger, click once, then leave the trackpad idle."
        Write-Host "Capturing ETW metadata for $TraceSeconds seconds..."
        Start-Sleep -Seconds $TraceSeconds
    }
    catch {
        Append-Utf8File -Path $wprLogPath `
            -Content (Protect-DiagnosticText $_.Exception.ToString())
    }
    finally {
        if ($wprStarted) {
            $stopOutput = & wpr.exe -stop $etlPath 2>&1 | Out-String
            Append-Utf8File -Path $wprLogPath `
                -Content (Protect-DiagnosticText $stopOutput)
        }
    }
}

$manifest = [ordered]@{
    schema = "magicpad-diagnostics/v1"
    collectedAtUtc = $captureStartedAt.ToUniversalTime().ToString("o")
    target = [ordered]@{
        osArchitecture = "x64"
        transport = "bluetooth"
        vendorId = "0x004c"
        productId = "0x0324"
    }
    targetDeviceCount = $targetDevices.Count
    sensitiveIdentifiersIncluded = [bool]$IncludeSensitiveIdentifiers
    traceSeconds = $TraceSeconds
    notes = @(
        "This collector does not change device state or send HID feature reports.",
        "ETW output contains metadata only; raw 0x31 touch frames require a separate test-only capture path."
    )
}
Write-Utf8File -Path (Join-Path $OutputDirectory "manifest.json") `
    -Content (ConvertTo-JsonText $manifest -Depth 8)

if ($targetDevices.Count -eq 0) {
    Write-Warning "No Bluetooth Magic Trackpad VID 004c / PID 0324 device was found."
}

$hashRecords = @(Get-ChildItem -LiteralPath $OutputDirectory -File | Sort-Object Name |
    ForEach-Object {
        $hash = Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256
        "{0}  {1}" -f $hash.Hash.ToLowerInvariant(), $_.Name
    })
Write-Utf8File -Path (Join-Path $OutputDirectory "hashes.sha256") `
    -Content ($hashRecords -join [Environment]::NewLine)

if (-not $NoArchive) {
    $archivePath = "$OutputDirectory.zip"
    if (Test-Path -LiteralPath $archivePath) {
        throw "Archive already exists: $archivePath"
    }
    Compress-Archive -Path (Join-Path $OutputDirectory "*") `
        -DestinationPath $archivePath -CompressionLevel Optimal
    Write-Host "Created $archivePath"
}

Write-Host "Diagnostics collection complete."
