# Administrator-only local test setup. Never installs a driver or clears TPM/UEFI keys.
[CmdletBinding()]
param(
    [Parameter(Mandatory)][ValidateSet('TrustPackage','EnterFirmware','EnableTestSigning','ResumeProtection')][string]$Action,
    [string]$PackagePath,
    [ValidateRange(0,60)][int]$DelaySeconds = 45
)
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$repo = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..\..'))
$logDirectory = Join-Path $repo 'diagnostics'
New-Item -ItemType Directory -Path $logDirectory -Force | Out-Null
$resultPath = Join-Path $logDirectory ("source-test-$Action.json")
$result = [ordered]@{action=$Action;startedAtUtc=(Get-Date).ToUniversalTime().ToString('o');phase='checking';error=$null}
$suspendedHere = $false
function Save-Result { $result | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath $resultPath -Encoding UTF8 }
function Get-SystemVolume {
    Get-CimInstance -Namespace root\CIMV2\Security\MicrosoftVolumeEncryption -ClassName Win32_EncryptableVolume | Where-Object DriveLetter -eq $env:SystemDrive
}
function Suspend-SystemProtection([int]$Reboots) {
    $volume = Get-SystemVolume
    if (-not $volume) { throw 'Could not determine system drive encryption state.' }
    $conversion = Invoke-CimMethod -InputObject $volume -MethodName GetConversionStatus
    if ($conversion.ReturnValue -ne 0) { throw 'Could not query encryption conversion status.' }
    if ($conversion.ConversionStatus -eq 0) { return }
    if ($conversion.ConversionStatus -ne 1) { throw 'System drive encryption is in transition; finish that operation first.' }
    & "$env:windir\System32\manage-bde.exe" -protectors -disable $env:SystemDrive -rebootcount $Reboots | Out-Null
    if ($LASTEXITCODE -ne 0 -or (Get-SystemVolume).ProtectionStatus -ne 0) { throw 'BitLocker suspension did not succeed; boot changes were not attempted.' }
    $script:suspendedHere = $true
    $result['bitLockerResumeAfterReboots'] = $Reboots
}
try {
    Save-Result
    $identity = [Security.Principal.WindowsIdentity]::GetCurrent()
    if (-not ([Security.Principal.WindowsPrincipal]$identity).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) { throw 'Run with administrator rights.' }
    if ([IntPtr]::Size -ne 8) { throw 'Use x64 PowerShell.' }
    $result['secureBoot'] = Confirm-SecureBootUEFI
    switch ($Action) {
        'TrustPackage' {
            if (-not $PackagePath) { throw '-PackagePath is required.' }
            $package = (Resolve-Path -LiteralPath $PackagePath).Path
            $manifest = Get-Content -LiteralPath (Join-Path $package 'manifest.json') -Raw | ConvertFrom-Json
            if ($manifest.schema -ne 'magicpad-source-test-package/v1' -or $manifest.hardwareId -ne 'BTHENUM\{00001124-0000-1000-8000-00805f9b34fb}_VID&0001004C_PID&0324') { throw 'Unexpected source package.' }
            foreach ($name in @('AmtPtpSource.sys','AmtPtpSource.inf','AmtPtpSource.cat','SourceTest.cer')) {
                if ((Get-FileHash -LiteralPath (Join-Path $package $name) -Algorithm SHA256).Hash -ne $manifest.files.$name) { throw "Package hash mismatch: $name" }
            }
            $certificatePath = Join-Path $package 'SourceTest.cer'
            $certificate = [Security.Cryptography.X509Certificates.X509Certificate2]::new($certificatePath)
            if ($certificate.Thumbprint -ne $manifest.certificateThumbprint -or $certificate.Subject -ne 'CN=MagicPad Source Local Hardware Test' -or $certificate.NotAfter -le (Get-Date)) { throw 'Unexpected or expired test certificate.' }
            $result['certificateThumbprint'] = $certificate.Thumbprint
            # Public certificate only. The non-exportable signing key stays in CurrentUser\My.
            foreach ($store in @('Cert:\LocalMachine\Root','Cert:\LocalMachine\TrustedPublisher')) {
                if (-not (Test-Path -LiteralPath (Join-Path $store $certificate.Thumbprint))) { Import-Certificate -FilePath $certificatePath -CertStoreLocation $store | Out-Null }
            }
            $signtool = Join-Path $repo 'packages\Microsoft.Windows.SDK.CPP.10.0.26100.6584\c\bin\10.0.26100.0\x64\signtool.exe'
            foreach ($name in @('AmtPtpSource.sys','AmtPtpSource.cat')) {
                $signature = Get-AuthenticodeSignature -LiteralPath (Join-Path $package $name)
                if ($signature.Status -ne 'Valid' -or $signature.SignerCertificate.Thumbprint -ne $certificate.Thumbprint) { throw "Signature verification failed: $name" }
                & $signtool verify /pa (Join-Path $package $name) | Out-Null
                if ($LASTEXITCODE -ne 0) { throw "Authenticode verification failed: $name" }
            }
            foreach ($name in @('AmtPtpSource.sys','AmtPtpSource.inf')) {
                & $signtool verify /pa /c (Join-Path $package 'AmtPtpSource.cat') (Join-Path $package $name) | Out-Null
                if ($LASTEXITCODE -ne 0) { throw "Catalog membership verification failed: $name" }
            }
            $result.phase = 'trustedAndVerified'
        }
        'EnterFirmware' {
            if (-not $result.secureBoot) { throw 'Secure Boot is already disabled; no firmware restart needed.' }
            Suspend-SystemProtection 2
            $result.phase = 'firmwareRestartScheduled'
            $result['restartAtUtc'] = (Get-Date).AddSeconds($DelaySeconds).ToUniversalTime().ToString('o')
            Save-Result
            Start-Sleep -Seconds $DelaySeconds
            if ((Get-SystemVolume).ProtectionStatus -eq 1) { throw 'Protection resumed before restart; aborting.' }
            # No /f and a zero shutdown timeout: do not force applications closed.
            & "$env:windir\System32\shutdown.exe" /r /fw /t 0
            if ($LASTEXITCODE -ne 0) { throw "Firmware restart failed: $LASTEXITCODE" }
            $result.phase = 'firmwareRestartRequested'
        }
        'EnableTestSigning' {
            if ($result.secureBoot) { throw 'Disable Secure Boot in firmware first.' }
            Suspend-SystemProtection 1
            & "$env:windir\System32\bcdedit.exe" /set testsigning on | Out-Null
            if ($LASTEXITCODE -ne 0) { throw "Enabling test signing failed: $LASTEXITCODE" }
            $result.phase = 'testSigningConfiguredRestartRequired'
        }
        'ResumeProtection' {
            & "$env:windir\System32\manage-bde.exe" -protectors -enable $env:SystemDrive | Out-Null
            if ($LASTEXITCODE -ne 0) { throw 'Could not resume BitLocker protection.' }
            $result['protectionStatus'] = (Get-SystemVolume).ProtectionStatus
            if ($result.protectionStatus -ne 1) { throw 'Protection is not active; check the drive encryption status.' }
            $result.phase = 'protectionResumed'
        }
    }
} catch {
    $result.phase = 'failed'
    $result.error = $_.Exception.Message
    if ($suspendedHere -and $Action -eq 'EnterFirmware') {
        & "$env:windir\System32\manage-bde.exe" -protectors -enable $env:SystemDrive | Out-Null
        $result['resumeAfterFailureExitCode'] = $LASTEXITCODE
    }
} finally {
    $result['updatedAtUtc'] = (Get-Date).ToUniversalTime().ToString('o')
    Save-Result
}
if ($result.phase -eq 'failed') { Write-Error $result.error; exit 1 }
