[CmdletBinding()]
param(
    [ValidateSet("x64", "ARM64")]
    [string]$Platform = "x64",

    [ValidateSet("Debug", "Release")]
    [string]$Configuration = "Release",

    [string]$CertificateSubject = "CN=MagicPad Driver Lab Test",

    [switch]$SkipBuild
)

$ErrorActionPreference = "Stop"

throw @"
Driver packaging is quarantined for this branch. The current INF still contains
the legacy NullDevice/filter registration and broad, unverified device bindings.
Do not sign or create an installable package until a dedicated 004C:0324
Bluetooth VHF INF and its filter ordering have passed review and hardware gates.
"@

$repositoryRoot = (Resolve-Path (Join-Path $PSScriptRoot "..\..")).Path
$outputDirectory = Join-Path $repositoryRoot "build\test-package\$Platform"
$packagesDirectory = Join-Path $repositoryRoot "packages"
$legacyDetourPath = Join-Path $repositoryRoot "AmtPtpHidFilter\Detour.c"
$legacyPrivateHeaderPath = Join-Path $repositoryRoot "AmtPtpHidFilter\include\Hac.h"
$filterProjectPath = Join-Path $repositoryRoot "AmtPtpHidFilter\AmtPtpHidFilter.vcxproj"
$requiredVhfSources = @(
    (Join-Path $repositoryRoot "AmtPtpHidFilter\VhfDevice.c"),
    (Join-Path $repositoryRoot "AmtPtpHidFilter\PhysicalHid.c"),
    (Join-Path $repositoryRoot "AmtPtpHidFilter\PtpReports.c")
)

if ($SkipBuild) {
    throw @"
-SkipBuild is disabled for driver packaging. A clean rebuild is required so a
stale legacy filter binary cannot be copied into a signed package.
"@
}

if ((Test-Path -LiteralPath $legacyDetourPath -PathType Leaf) -or
    (Test-Path -LiteralPath $legacyPrivateHeaderPath -PathType Leaf)) {
    throw @"
Driver packaging is disabled while the legacy detour/private-layout sources are present.
The legacy filter changes another driver's shared dispatch table and is not a
supported release architecture. Complete the VHF migration and remove the
detour/private-layout dependency before creating a signed or installable package.
"@
}

$missingVhfSources = @($requiredVhfSources | Where-Object {
    -not (Test-Path -LiteralPath $_ -PathType Leaf)
})
if ($missingVhfSources.Count -ne 0) {
    throw "Driver packaging requires the completed VHF source set: $($missingVhfSources -join ', ')"
}

$filterProjectText = [System.IO.File]::ReadAllText($filterProjectPath)
if ($filterProjectText -match '(?i)(?:Detour\.c|include[\\/]Hac\.h)' -or
    $filterProjectText -notmatch '(?i)\bVhfKm\.lib\b' -or
    $filterProjectText -notmatch '(?i)\bVhfDevice\.c\b' -or
    $filterProjectText -notmatch '(?i)\bPhysicalHid\.c\b' -or
    $filterProjectText -notmatch '(?i)\bPtpReports\.c\b') {
    throw @"
AmtPtpHidFilter.vcxproj is not release-ready. It must exclude Detour/Hac, include
the VHF/physical-HID/PTP source set, and link VhfKm.lib before packaging.
"@
}

function Find-KitTool {
    param(
        [Parameter(Mandatory = $true)]
        [string]$RelativePath
    )

    $kitsRoot = Join-Path ${env:ProgramFiles(x86)} "Windows Kits\10\bin"
    $tool = Get-ChildItem $kitsRoot -Directory |
        Sort-Object Name -Descending |
        ForEach-Object { Join-Path $_.FullName $RelativePath } |
        Where-Object { Test-Path $_ } |
        Select-Object -First 1

    if (-not $tool) {
        throw "Cannot find $RelativePath under $kitsRoot"
    }
    return $tool
}

function Invoke-Checked {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Command,

        [Parameter(Mandatory = $true)]
        [string[]]$Arguments
    )

    & $Command @Arguments
    if ($LASTEXITCODE -ne 0) {
        throw "$Command failed with exit code $LASTEXITCODE"
    }
}

if (-not (Get-Command nuget.exe -ErrorAction SilentlyContinue)) {
    throw "nuget.exe is not available on PATH"
}
if (-not (Get-Command msbuild.exe -ErrorAction SilentlyContinue)) {
    throw "msbuild.exe is not available on PATH"
}

Invoke-Checked -Command nuget.exe -Arguments @(
    "restore",
    (Join-Path $repositoryRoot "AmtPtpDeviceUsbUm\MagicTrackpad2PtpDevice.vcxproj"),
    "-PackagesDirectory",
    $packagesDirectory
)
Invoke-Checked -Command nuget.exe -Arguments @(
    "restore",
    (Join-Path $repositoryRoot "AmtPtpHidFilter\AmtPtpHidFilter.vcxproj"),
    "-PackagesDirectory",
    $packagesDirectory
)

Invoke-Checked -Command msbuild.exe -Arguments @(
    (Join-Path $repositoryRoot "AmtPtpDeviceUsbUm\MagicTrackpad2PtpDevice.vcxproj"),
    "/t:Rebuild",
    "/p:Configuration=$Configuration",
    "/p:Platform=$Platform",
    "/p:ApiValidator_Enable=false"
)
Invoke-Checked -Command msbuild.exe -Arguments @(
    (Join-Path $repositoryRoot "AmtPtpHidFilter\AmtPtpHidFilter.vcxproj"),
    "/t:Rebuild",
    "/p:Configuration=$Configuration",
    "/p:Platform=$Platform",
    "/p:ApiValidator_Enable=false"
)

if (Test-Path -LiteralPath $outputDirectory) {
    # Platform is ValidateSet-constrained and outputDirectory is rooted below
    # build\test-package, so a future packaging run cannot retain stale files.
    Remove-Item -LiteralPath $outputDirectory -Recurse -Force
}
New-Item -ItemType Directory -Path $outputDirectory | Out-Null

$infName = if ($Platform -eq "x64") {
    "AmtPtpDevice_AMD64.inf"
} else {
    "AmtPtpDevice_ARM64.inf"
}
$usbDriver = Join-Path $repositoryRoot `
    "AmtPtpDeviceUsbUm\build\AmtPtpDeviceUsbUm\$Platform\$Configuration\AmtPtpDeviceUsbUm.dll"
$filterDriver = Join-Path $repositoryRoot `
    "AmtPtpHidFilter\build\AmtPtpHidFilter\$Platform\$Configuration\AmtPtpHidFilter.sys"

foreach ($source in @(
    (Join-Path $repositoryRoot "build\$infName"),
    $usbDriver,
    $filterDriver
)) {
    if (-not (Test-Path $source)) {
        throw "Missing build artifact: $source"
    }
}

Copy-Item (Join-Path $repositoryRoot "build\$infName") `
    (Join-Path $outputDirectory "AmtPtpDevice.inf") -Force
Copy-Item $usbDriver $outputDirectory -Force
Copy-Item $filterDriver $outputDirectory -Force

$certificate = Get-ChildItem Cert:\CurrentUser\My |
    Where-Object {
        $_.Subject -eq $CertificateSubject -and
        $_.NotAfter -gt (Get-Date).AddDays(30)
    } |
    Sort-Object NotAfter -Descending |
    Select-Object -First 1

if (-not $certificate) {
    $certificate = New-SelfSignedCertificate `
        -Type CodeSigningCert `
        -Subject $CertificateSubject `
        -CertStoreLocation Cert:\CurrentUser\My `
        -HashAlgorithm SHA256 `
        -KeyAlgorithm RSA `
        -KeyLength 3072 `
        -NotAfter (Get-Date).AddYears(2)
}

$certificatePath = Join-Path $outputDirectory "MagicPadDriverLabTest.cer"
Export-Certificate -Cert $certificate -FilePath $certificatePath -Force | Out-Null

$signTool = Find-KitTool "x64\signtool.exe"
$inf2Cat = Find-KitTool "x86\inf2cat.exe"
$thumbprint = $certificate.Thumbprint

foreach ($binary in @(
    (Join-Path $outputDirectory "AmtPtpDeviceUsbUm.dll"),
    (Join-Path $outputDirectory "AmtPtpHidFilter.sys")
)) {
    Invoke-Checked -Command $signTool -Arguments @(
        "sign", "/v", "/fd", "SHA256", "/s", "My", "/sha1", $thumbprint, $binary
    )
}

$inf2CatOs = if ($Platform -eq "x64") { "10_X64" } else { "10_RS3_ARM64" }
Invoke-Checked -Command $inf2Cat -Arguments @(
    "/driver:$outputDirectory",
    "/os:$inf2CatOs"
)
Invoke-Checked -Command $signTool -Arguments @(
    "sign",
    "/v",
    "/fd",
    "SHA256",
    "/s",
    "My",
    "/sha1",
    $thumbprint,
    (Join-Path $outputDirectory "AmtPtpDevice.cat")
)

Write-Host ""
Write-Host "Test-signed package created:" -ForegroundColor Green
Write-Host "  $outputDirectory"
Write-Host ""
Write-Host "The script did not trust the certificate, enable Test Mode, or install the driver."
Write-Host "Follow docs\windows-testing.md from an elevated terminal."
