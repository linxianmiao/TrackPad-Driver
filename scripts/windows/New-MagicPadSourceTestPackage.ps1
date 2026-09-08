# Builds only AmtPtpSource. No trust-store, PnP, or boot configuration changes.
[CmdletBinding()]
param(
    [switch]$EnableHardwareTest,
    [string]$MSBuildPath
)
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
if (-not $EnableHardwareTest) { throw 'Local prototype testing must be explicitly enabled with -EnableHardwareTest.' }
if ([IntPtr]::Size -ne 8) { throw 'Use x64 PowerShell.' }
$repo = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..\..'))
$wdk = Join-Path $repo 'packages\Microsoft.Windows.WDK.x64.10.0.26100.6584\c'
$sdk = Join-Path $repo 'packages\Microsoft.Windows.SDK.CPP.10.0.26100.6584\c'
$infverif = Join-Path $wdk 'tools\10.0.26100.0\x64\infverif.exe'
$inf2cat = Join-Path $wdk 'bin\10.0.26100.0\x86\Inf2Cat.exe'
$signtool = Join-Path $sdk 'bin\10.0.26100.0\x64\signtool.exe'
if (-not $MSBuildPath) {
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    $MSBuildPath = & $vswhere -latest -products '*' -requires Microsoft.Component.MSBuild -find 'MSBuild\**\Bin\MSBuild.exe' | Select-Object -First 1
}
foreach ($tool in @($MSBuildPath, $infverif, $inf2cat, $signtool)) {
    if (-not $tool -or -not (Test-Path -LiteralPath $tool -PathType Leaf)) { throw "Missing build tool: $tool" }
}
function Invoke-Checked([string]$Tool, [string[]]$Arguments) {
    & $Tool @Arguments | Out-Host
    if ($LASTEXITCODE -ne 0) { throw "$Tool failed with exit code $LASTEXITCODE" }
}
Push-Location $repo
try {
    Invoke-Checked 'node' @('scripts/check-driver-contracts.mjs')
    Invoke-Checked $MSBuildPath @('AmtPtpSource\AmtPtpSource.vcxproj', '/t:Rebuild', '/p:Configuration=Release', '/p:Platform=x64', '/p:SignMode=Off', '/p:RunCodeAnalysis=true', '/v:minimal', '/nologo')
    # Never overwrite or delete a previous package; each build has its own directory.
    $package = Join-Path $repo ('diagnostics\source-test-' + (Get-Date -Format 'yyyyMMdd-HHmmss'))
    New-Item -ItemType Directory -Path $package -ErrorAction Stop | Out-Null
    Copy-Item -LiteralPath (Join-Path $repo 'AmtPtpSource\build\x64\Release\AmtPtpSource.sys') -Destination $package
    Copy-Item -LiteralPath (Join-Path $repo 'AmtPtpSource\AmtPtpSource.inf.in') -Destination (Join-Path $package 'AmtPtpSource.inf')
    Invoke-Checked $infverif @('/w', '/v', (Join-Path $package 'AmtPtpSource.inf'))

    $subject = 'CN=MagicPad Source Local Hardware Test'
    $certificates = @(Get-ChildItem Cert:\CurrentUser\My | Where-Object {
        $_.Subject -eq $subject -and $_.HasPrivateKey -and $_.NotAfter -gt (Get-Date).AddDays(1)
    })
    if ($certificates.Count -gt 1) { throw 'Multiple local source test certificates found; resolve before signing.' }
    if ($certificates.Count -eq 1) { $certificate = $certificates[0] }
    else {
        $certificate = New-SelfSignedCertificate -Type CodeSigningCert -Subject $subject -FriendlyName 'MagicPad source local test only' -CertStoreLocation Cert:\CurrentUser\My -KeyAlgorithm RSA -KeyLength 2048 -HashAlgorithm SHA256 -KeyExportPolicy NonExportable -NotAfter (Get-Date).AddDays(30)
    }
    Export-Certificate -Cert $certificate -FilePath (Join-Path $package 'SourceTest.cer') | Out-Null
    Invoke-Checked $signtool @('sign', '/s', 'My', '/sha1', $certificate.Thumbprint, '/fd', 'SHA256', (Join-Path $package 'AmtPtpSource.sys'))
    Invoke-Checked $inf2cat @("/driver:$package", '/os:10_CO_X64,10_NI_X64,10_GE_X64', '/uselocaltime')
    Invoke-Checked $signtool @('sign', '/s', 'My', '/sha1', $certificate.Thumbprint, '/fd', 'SHA256', (Join-Path $package 'AmtPtpSource.cat'))
    foreach ($name in @('AmtPtpSource.sys', 'AmtPtpSource.cat')) {
        $signature = Get-AuthenticodeSignature -LiteralPath (Join-Path $package $name)
        if (-not $signature.SignerCertificate -or $signature.SignerCertificate.Thumbprint -ne $certificate.Thumbprint) { throw "Unexpected signer: $name" }
        # Full chain/catalog verification runs after explicit local machine trust setup.
        if ($signature.Status -notin @('Valid', 'UnknownError', 'NotTrusted')) { throw "Signature failed: $name ($($signature.Status))" }
    }
    $files = [ordered]@{}
    foreach ($name in @('AmtPtpSource.sys','AmtPtpSource.inf','AmtPtpSource.cat','SourceTest.cer')) {
        $files[$name] = (Get-FileHash -LiteralPath (Join-Path $package $name) -Algorithm SHA256).Hash
    }
    $manifest = [ordered]@{
        schema='magicpad-source-test-package/v1'
        createdAtUtc=(Get-Date).ToUniversalTime().ToString('o')
        commit=(& git rev-parse HEAD)
        workingTreeModified=[bool](& git status --porcelain --untracked-files=no)
        hardwareId='BTHENUM\{00001124-0000-1000-8000-00805f9b34fb}_VID&0001004C_PID&0324'
        certificateThumbprint=$certificate.Thumbprint
        certificateExpires=$certificate.NotAfter.ToUniversalTime().ToString('o')
        files=$files
    }
    $manifest | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $package 'manifest.json') -Encoding UTF8
    Write-Output $package
} finally { Pop-Location }
