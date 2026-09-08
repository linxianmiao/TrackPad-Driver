# MagicPad Native PTP Driver

English | [简体中文](README.md)

Use a **2024 USB-C Apple Magic Trackpad over Bluetooth on Windows 11 x64** with native Windows Precision Touchpad (PTP) gestures and battery level in Bluetooth settings.

> This is the development test version, `AmtPtpSource 0.2.0.6`. No ready-to-install production package signed by Microsoft is available.
> The instructions below cover building, test-signing, and installing on a local test machine. They change certificate trust and boot settings and are not suitable for a daily-use primary PC unless you accept the testing risks.

## System and device requirements

| Item | Requirements and validation scope |
| --- | --- |
| Windows | Native **Windows 11 x64 (Intel / AMD)**. The INF matches build 22000 and later; local hardware testing used build 26100. This does not mean every Windows 11 version has been validated. |
| Trackpad | **2024 USB-C Magic Trackpad**, connected over **Bluetooth**, with Bluetooth VID/PID `004c:0324`. |
| Unsupported or unverified | Windows 10, Windows ARM64, virtual machines, older Lightning / first-generation Trackpads, and wired USB input are outside the installation scope of this version. USB-C identifies the device generation; it does not mean this driver uses a USB connection. |
| Test boot configuration | This procedure uses Secure Boot disabled and TESTSIGNING enabled. A “Test Mode” desktop watermark is expected. |
| HVCI / Memory integrity | It was not running during local validation; operation with it enabled has not been validated. If it is enabled, stop at read-only diagnostics rather than disabling it to work around compatibility problems. |
| Installation prerequisites | Administrator rights and a working backup keyboard/mouse. Before changing boot settings, make sure you can retrieve the BitLocker / device encryption recovery key. Never upload the key to the repository or logs. |

The new driver matches only this Bluetooth HID service, not all Apple or Bluetooth devices:

```text
BTHENUM\{00001124-0000-1000-8000-00805f9b34fb}_VID&0001004C_PID&0324
```

## Validated features and limitations

- Single-finger pointer movement, press-to-click, two-finger scrolling, and zooming.
- Three-finger Task View / show desktop / window switching, and four-finger virtual desktop switching. The exact actions depend on Windows touchpad settings.
- Actual battery level in the Bluetooth settings list, queried approximately every 60 seconds, with no additional tray application.

The full gesture validation was performed with `0.2.0.2`; touch input and battery display were rechecked with the current `0.2.0.6` (the local battery reading was 100%). Sleep/wake, disconnect/reconnect, uninstall recovery, Driver Verifier, and long-term stability remain unvalidated. Battery level changes, charging status, and other PCs / Bluetooth adapters must not be treated as validated.

## Installation (local testing only)

Follow these steps in order and stop if any step fails. Run the scripts and commands below from the **repository root in 64-bit PowerShell**. Steps marked “administrator” require an elevated session.

### 1. Prepare the build environment

Install the following on Windows:

- Git, Node.js 22, and NuGet CLI, with their commands available in PATH.
- Visual Studio 2022 / Build Tools 2022 (MSBuild 17), Desktop development with C++, v143 x64/x86 Spectre-mitigated libraries, and Windows Driver Kit build integration components.
- The project pins SDK / WDK `10.0.26100.6584` through NuGet. Installing a different system WDK does not replace dependency restoration.

Open **Developer PowerShell (x64)** for VS 2022, clone the source, and restore dependencies:

```powershell
git clone --branch master --single-branch https://github.com/linxianmiao/TrackPad-Driver.git
if ($LASTEXITCODE -ne 0) { throw 'Clone failed; stop here.' }
cd TrackPad-Driver
nuget restore .\AmtPtpSource\AmtPtpSource.vcxproj -PackagesDirectory .\packages
if ($LASTEXITCODE -ne 0) { throw 'Dependency restore failed; stop here.' }
```

Generate the dedicated test package:

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\scripts\windows\New-MagicPadSourceTestPackage.ps1 -EnableHardwareTest
```

This rebuilds `AmtPtpSource`, validates the INF, generates the CAT, and signs the SYS/CAT. It creates or reuses a local test certificate (new certificates are valid for 30 days and have a non-exportable private key). **It does not install a driver, change boot settings, or add the certificate to machine trust stores.**

Save the full package directory printed at the end: `diagnostics\source-test-YYYYMMDD-HHMMSS`. The package contains the SYS, INF, CAT, `SourceTest.cer`, and `manifest.json`. Do not manually modify package files or mistake the read-only diagnostics EXE from CI for a driver installer.

### 2. Prepare the test boot configuration (administrator)

Save your work and have backup input devices and the encryption recovery key ready. If Magic Utilities, Trackpad++, or another replacement Trackpad driver is installed, use its normal uninstall procedure first and confirm that basic pointer input works with the built-in Microsoft driver.

In an elevated 64-bit PowerShell session, navigate to the repository root and set the variable to the **actual absolute directory printed in the previous step**. Replace the example path. Set this variable again whenever you open a new session or restart Windows:

```powershell
$magicPadPackage = 'C:\path\to\TrackPad-Driver\diagnostics\source-test-YYYYMMDD-HHMMSS'
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\scripts\windows\Prepare-MagicPadSourceTest.ps1 -Action TrustPackage -PackagePath "$magicPadPackage"
```

`TrustPackage` checks package hashes, the certificate, and signatures, imports the package's public certificate into the machine Root / TrustedPublisher stores, and then verifies catalog membership. Only trust packages you built yourself and whose origin you have confirmed.

First, check boot and disk status without making changes:

```powershell
Confirm-SecureBootUEFI
manage-bde -status
```

If Secure Boot is still enabled, the following command suspends BitLocker protection on a protected system drive (limited to two Windows restarts) and **restarts into firmware after a default 45-second delay**:

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\scripts\windows\Prepare-MagicPadSourceTest.ps1 -Action EnterFirmware
```

In UEFI, disable only Secure Boot, save, and return to Windows. Do not clear the TPM, remove Secure Boot keys, or switch to Legacy / CSM. Skip `EnterFirmware` if Secure Boot is already disabled. If the script cannot determine encryption or firmware status, stop rather than bypassing its checks.

After returning to Windows, run the following as administrator from the repository root:

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\scripts\windows\Prepare-MagicPadSourceTest.ps1 -Action EnableTestSigning
```

This enables TESTSIGNING and suspends protection on a protected system drive for one restart. **It does not restart automatically.** Save your work and restart manually. After booting, check for Test Mode and use `manage-bde -status` to confirm the protection status of all encrypted drives. Run the following only if protection on the encrypted system drive is still suspended:

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\scripts\windows\Prepare-MagicPadSourceTest.ps1 -Action ResumeProtection
```

The script resumes protection only on the system drive; check other encrypted drives separately. Do not run this action unconditionally on an unencrypted system drive. See [Microsoft's test-signing documentation](https://learn.microsoft.com/en-us/windows-hardware/drivers/install/the-testsigning-boot-configuration-option) for Test Mode and restart requirements, and the [BitLocker documentation](https://learn.microsoft.com/en-us/windows-server/administration/windows-commands/manage-bde-protectors) for protection suspension.

### 3. Confirm the device and install (administrator)

Pair the Trackpad over Bluetooth in Windows Settings; do not test input through a USB cable. Open a new elevated 64-bit PowerShell session, navigate to the repository root, and set `$magicPadPackage` again as shown above.

Locate exactly one matching Bluetooth HID service without making changes:

```powershell
$magicPadHardwareId = 'BTHENUM\{00001124-0000-1000-8000-00805f9b34fb}_VID&0001004C_PID&0324'
$magicPadTargets = @(Get-PnpDevice -PresentOnly |
    Where-Object { $_.InstanceId -like 'BTHENUM\*' } |
    Where-Object {
        $magicPadIds = (Get-PnpDeviceProperty -InstanceId $_.InstanceId `
            -KeyName DEVPKEY_Device_HardwareIds -ErrorAction Stop).Data
        @($magicPadIds) -contains $magicPadHardwareId
    })
if ($magicPadTargets.Count -ne 1) { throw 'Expected exactly one target Bluetooth HID service.' }
$magicPadInstanceId = $magicPadTargets[0].InstanceId
pnputil /enum-devices /instanceid "$magicPadInstanceId" /drivers
```

Before a first installation, confirm that the target uses Microsoft HidBth / `hidbth.inf` and that basic pointer movement works with the Microsoft driver. Stop if the target count is not one, its identity is unclear, or it is still bound to another third-party driver. Do not publish the full device instance ID in public logs.

Revalidate the same package immediately before installation, then install:

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\scripts\windows\Prepare-MagicPadSourceTest.ps1 -Action TrustPackage -PackagePath "$magicPadPackage"
if ($LASTEXITCODE -ne 0) { throw 'Package verification failed; installation aborted.' }
pnputil /add-driver "$magicPadPackage\AmtPtpSource.inf" /install
if ($LASTEXITCODE -ne 0) { throw 'Review the PnPUtil result, including any restart request, before continuing.' }
Get-PnpDeviceProperty -InstanceId $magicPadInstanceId -KeyName DEVPKEY_Device_Service,DEVPKEY_Device_DriverInfPath
```

Record the actual published name, `oemNN.inf`, and confirm that the target service is `AmtPtpSource`. If a restart is requested, restart first, then locate the target again and check its service. Adding a package to the Driver Store with PnPUtil **does not mean the device has been bound to it**; PnPUtil will not forcibly replace a higher-ranked driver. If the device still uses HidBth or installation fails, retain the output and diagnostics and stop. Do not force the driver onto other devices or change global Bluetooth/HID filters. See the [PnPUtil documentation](https://learn.microsoft.com/en-us/windows-hardware/drivers/devtest/pnputil-command-syntax).

### 4. Validate

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\scripts\windows\Read-MagicPadSourceStatus.ps1
```

After touching the Trackpad, expect `Connected=1`, `ModeEnabled=1`, and `InputMode=3`, increasing `TouchPackets` / `Reports` counters, and `InvalidPackets=0`. `sourceNotPresent` means the new driver's interface was not found; it does not necessarily mean the device model is incompatible.

Open Windows **Settings → Bluetooth & devices → Touchpad** and test movement, clicking, and two-/three-/four-finger gestures. Check battery level in the Bluetooth device list. Battery queries run once per minute, and the UI may take additional time to refresh. Counters and a device appearing in Settings are not substitutes for actual gesture testing.

## Uninstall and restore

Keep backup input devices available. In an elevated PowerShell session, use `pnputil /enum-drivers` to find and verify the original name `AmtPtpSource.inf`, provider `MagicPad Native PTP Project`, and the published name actually used by the target in the previous step.

Replace `oemNN.inf` below only with **the package you have verified**. Do not use wildcards or `/force`:

```powershell
pnputil /delete-driver oemNN.inf /uninstall
pnputil /scan-devices
```

Restart if Windows requests it, confirm that the device returns to Microsoft HidBth, and test basic pointer input. This recovery procedure has not yet completed hardware validation; automatic recovery from every failure is not guaranteed. If the prototype causes boot problems, turn off the external Trackpad first, then use Windows Recovery Environment / Safe Mode to address the problem. Do not delete Bluetooth drivers in bulk. See [Microsoft's uninstall documentation](https://learn.microsoft.com/en-us/windows-hardware/drivers/install/using-device-manager-to-uninstall-devices-and-driver-packages).

After confirming that the prototype has been removed and no other test drivers depend on TESTSIGNING, arrange a limited-restart BitLocker suspension for the protected system drive before running:

```powershell
bcdedit /set testsigning off
```

Restart for the change to take effect, then use Windows Advanced startup to enter UEFI and re-enable Secure Boot. Finally, confirm that protection has resumed on every encrypted drive. Do not use this project's `EnterFirmware` action to restore Secure Boot: that action refuses to run when Secure Boot is already disabled. After confirming that no other package uses the test certificate, use the exact thumbprint from `manifest.json` to remove its Root / TrustedPublisher trust entries and CurrentUser\My private key. Do not delete certificates in bulk based on a generic name.

## Development and further reading

`AmtPtpSource` is the new Bluetooth L2CAP + VHF driver. The old `AmtPtpHidFilter`, `AmtPtpDeviceUsbUm`, and control panel are retained only as historical references; they are not installation targets in this README. **Do not use the old `build/make*.bat` files or `New-TestSignedPackage.ps1` to install old packages.**

Cross-platform core and tool tests (require a C compiler, make, and Node.js 22):

```sh
make -C core test all
node scripts/check-driver-contracts.mjs
node --test scripts/tests/*.test.mjs
```

Optional local simulator: run `npm ci` and `npm run dev` in the `simulator` directory, then open [localhost:4173](http://127.0.0.1:4173). The simulator and CI cannot replace Windows hardware validation.

- [Native driver implementation and limitations](docs/native-ptp-source.md)
- [Local hardware test records and recovery details](docs/source-hardware-test.md)
- [Battery queries and Windows display](docs/battery-display.md)
- [AI handoff guide](docs/ai-handoff.md) (contains historical snapshots; prioritize the new source driver documentation and actual code)

## Source and license

The project is based on [vitoplantamura/MagicTrackpad2ForWindows@68b31c4](https://github.com/vitoplantamura/MagicTrackpad2ForWindows/commit/68b31c466f4e2ec8905cf7be44580b01705650f3), which derives from [imbushuo/mac-precision-touchpad](https://github.com/imbushuo/mac-precision-touchpad). Retained derivative code remains subject to [GPLv2](https://www.gnu.org/licenses/old-licenses/gpl-2.0.html); reorganizing Git history does not change code provenance or license terms. This project has not copied or reverse-engineered Magic Utilities' proprietary implementation.
