import assert from "node:assert/strict";
import { createHash } from "node:crypto";
import { readFileSync } from "node:fs";
import { fileURLToPath } from "node:url";
import { dirname, join } from "node:path";

const scriptDirectory = dirname(fileURLToPath(import.meta.url));
const repositoryRoot = join(scriptDirectory, "..");
const expectedBlobHash =
  "b57b851d567808906f61a0122273da05c972140f1007355390aaf5dda8a072af";

const contracts = [
  {
    name: "Bluetooth KMDF",
    header: "AmtPtpHidFilter/include/Metadata/WindowsHID.h",
    implementation: "AmtPtpHidFilter/Hid.c",
  },
  {
    name: "USB UMDF",
    header: "AmtPtpDeviceUsbUm/include/Hid.h",
    implementation: "AmtPtpDeviceUsbUm/Hid.c",
  },
];

function readRepositoryFile(relativePath) {
  return readFileSync(join(repositoryRoot, relativePath), "utf8").replace(/\r\n/gu, "\n");
}

function extractDefaultCertificationBlob(source, sourceName) {
  const lines = source.split(/\r?\n/u);
  const firstLine = lines.findIndex((line) =>
    line.startsWith("#define DEFAULT_PTP_HQA_BLOB"),
  );
  assert.notEqual(firstLine, -1, `${sourceName}: certification blob macro missing`);

  const macroLines = [];
  for (let index = firstLine + 1; index < lines.length; index += 1) {
    const line = lines[index];
    macroLines.push(line);
    if (!line.trimEnd().endsWith("\\")) {
      break;
    }
  }

  const bytes = [...macroLines.join("\n").matchAll(/0x([0-9a-f]{2})/giu)].map(
    (match) => Number.parseInt(match[1], 16),
  );
  assert.equal(bytes.length, 256, `${sourceName}: blob must contain 256 bytes`);
  return Buffer.from(bytes);
}

const blobs = contracts.map((contract) => {
  const header = readRepositoryFile(contract.header);
  const implementation = readRepositoryFile(contract.implementation);
  const blob = extractDefaultCertificationBlob(header, contract.header);

  assert.doesNotMatch(
    implementation,
    /\*\s*[^;\n]*CertificationBlob\s*=/u,
    `${contract.implementation}: single-byte blob assignment is forbidden`,
  );
  assert.match(
    implementation,
    /C_ASSERT\s*\(\s*sizeof\s*\(\s*certificationBlob\s*\)\s*==[\s\S]*?CertificationBlob\s*\)\s*\)/u,
    `${contract.implementation}: certification blob size assertion missing`,
  );
  assert.match(
    implementation,
    /RtlCopyMemory\s*\(\s*[^,;]*CertificationBlob\s*,\s*certificationBlob\s*,\s*sizeof\s*\(\s*certificationBlob\s*\)\s*\)/su,
    `${contract.implementation}: full certification blob copy missing`,
  );

  if (contract.name === "Bluetooth KMDF") {
    const getFeatures = implementation.match(
      /PtpFilterGetHidFeatures\s*\([\s\S]*?\n\}\n\nNTSTATUS\nPtpFilterSetHidFeatures/u,
    );
    assert.ok(
      getFeatures,
      `${contract.implementation}: GET_FEATURE implementation missing`,
    );
    assert.match(
      getFeatures[0],
      /WdfRequestSetInformation\s*\(\s*Request\s*,\s*reportSize\s*\)/u,
      `${contract.implementation}: successful GET_FEATURE must report returned bytes`,
    );
  }

  const hash = createHash("sha256").update(blob).digest("hex");
  assert.equal(hash, expectedBlobHash, `${contract.header}: unexpected blob bytes`);
  return { ...contract, blob };
});

assert.deepEqual(
  blobs[0].blob,
  blobs[1].blob,
  "Bluetooth and USB certification blobs must remain identical",
);

const probePath = "tools/windows/MagicPadHidProbe/MagicPadHidProbe.cpp";
const probe = readRepositoryFile(probePath);
assert.match(
  probe,
  /CreateFileW\s*\(\s*interfacePath\.c_str\(\)\s*,\s*0\s*,/u,
  `${probePath}: target interfaces must be opened with desired access 0`,
);
for (const forbiddenApi of [
  "ReadFile",
  "WriteFile",
  "DeviceIoControl",
  "HidD_GetInputReport",
  "HidD_GetFeature",
  "HidD_SetFeature",
  "HidD_SetOutputReport",
  "HidD_FlushQueue",
  "HidD_SetNumInputBuffers",
  "HidD_GetSerialNumberString",
  "HidD_GetManufacturerString",
  "HidD_GetProductString",
]) {
  assert.doesNotMatch(
    probe,
    new RegExp(`\\b${forbiddenApi}\\s*\\(`, "u"),
    `${probePath}: report I/O API ${forbiddenApi} is forbidden`,
  );
}

const workflowPath = ".github/workflows/build.yml";
const workflow = readRepositoryFile(workflowPath);
assert.doesNotMatch(
  workflow,
  /\bmakecab\b|drivers_(?:x64|arm64)\.cab/iu,
  `${workflowPath}: legacy detour driver packages must not be published`,
);
assert.match(
  workflow,
  /diagnostics-x64:[\s\S]*?needs:\s*portable-core[\s\S]*?Upload read-only bring-up tools/u,
  `${workflowPath}: diagnostics upload must wait for source contracts`,
);
assert.match(
  workflow,
  /legacy-compile:[\s\S]*?runs-on:\s*windows-2022[\s\S]*?vs-version:\s*'\[17\.0,18\.0\)'/u,
  `${workflowPath}: NuGet WDK compile must use Windows 2022 and MSBuild 17`,
);

const collectorPath = "scripts/windows/Collect-MagicTrackpadDiagnostics.ps1";
const collector = readRepositoryFile(collectorPath);
assert.match(
  collector,
  /\(\?:HID\|BTH\|BTHENUM\|BTHLEDEVICE\|USB\|PCI\)\(\?<instanceSep>/u,
  `${collectorPath}: BTH instance IDs must be redacted by default`,
);

const preflightPath = "scripts/windows/Test-MagicTrackpadTransport.ps1";
const preflight = readRepositoryFile(preflightPath);
for (const forbiddenApi of [
  "ReadFile", "WriteFile", "DeviceIoControl", "HidD_SetFeature",
  "HidD_GetFeature", "HidD_GetInputReport", "HidD_SetOutputReport",
  "HidD_FlushQueue", "HidD_GetSerialNumberString",
]) {
  assert.doesNotMatch(preflight, new RegExp(`\\b${forbiddenApi}\\s*\\(`, "u"),
    `${preflightPath}: preflight must not issue report I/O or collect serials`);
}

const sourceHqa = extractDefaultCertificationBlob(readRepositoryFile("core/include/amtptp_hqa.h"), "native source HQA");
assert.equal(createHash("sha256").update(sourceHqa).digest("hex"), expectedBlobHash);
const sourceProject = readRepositoryFile("AmtPtpSource/AmtPtpSource.vcxproj");
assert.match(sourceProject, /<SignMode>Off<\/SignMode>/u);
assert.match(sourceProject, /<Target Name="ValidateSourceDriverApis"/u);
assert.doesNotMatch(sourceProject, /Detour\.c|Hac\.h|AmtPtpHidFilter/u);
for (const path of ["AmtPtpSource/Driver.c", "AmtPtpSource/Bluetooth.c", "AmtPtpSource/Battery.c", "AmtPtpSource/Vhf.c", "AmtPtpSource/Source.h"]) {
  assert.doesNotMatch(readRepositoryFile(path), /MajorFunction\s*\[|#include.*(?:Detour|Hac)\b/u);
}
const sourceInf = readRepositoryFile("AmtPtpSource/AmtPtpSource.inf.in");
const sourceBindings = sourceInf.split("\n").filter((line) => line.startsWith("%DeviceDesc%="));
assert.deepEqual(sourceBindings, ["%DeviceDesc%=Source,BTHENUM\\{00001124-0000-1000-8000-00805f9b34fb}_VID&0001004C_PID&0324"]);

const packageScriptPath = "scripts/windows/New-TestSignedPackage.ps1";
const packageScript = readRepositoryFile(packageScriptPath);
assert.match(
  packageScript,
  /\$ErrorActionPreference\s*=\s*"Stop"\s*throw\s+@"[\s\S]*?packaging is quarantined/u,
  `${packageScriptPath}: packaging must remain unconditionally quarantined`,
);
assert.match(
  packageScript,
  /if \(\$SkipBuild\) \{\s*throw/su,
  `${packageScriptPath}: stale-build packaging bypass must be disabled`,
);
assert.match(
  packageScript,
  /if \(\(Test-Path -LiteralPath \$legacyDetourPath -PathType Leaf\) -or\s*\(Test-Path -LiteralPath \$legacyPrivateHeaderPath -PathType Leaf\)\) \{\s*throw/su,
  `${packageScriptPath}: packaging must fail while Detour/Hac is present`,
);
for (const requiredMarker of [
  "VhfDevice.c",
  "PhysicalHid.c",
  "PtpReports.c",
  "VhfKm.lib",
]) {
  assert.match(
    packageScript,
    new RegExp(requiredMarker.replace(".", "\\."), "u"),
    `${packageScriptPath}: release gate must require ${requiredMarker}`,
  );
}
assert.equal(
  [...packageScript.matchAll(/"\/t:Rebuild"/gu)].length,
  2,
  `${packageScriptPath}: both driver projects must be rebuilt before packaging`,
);

const filterDevicePath = "AmtPtpHidFilter/Device.c";
const filterDevice = readRepositoryFile(filterDevicePath);
const synchronousSendCount = [
  ...filterDevice.matchAll(/WDF_REQUEST_SEND_OPTION_SYNCHRONOUS/gu),
].length;
const explicitRequestSendCount = [
  ...filterDevice.matchAll(/\bWdfRequestSend\s*\(/gu),
].length;
assert.equal(
  [...filterDevice.matchAll(/WDF_REQUEST_SEND_OPTIONS_SET_TIMEOUT\s*\(/gu)].length,
  synchronousSendCount,
  `${filterDevicePath}: every synchronous lower request must have a timeout`,
);
assert.equal(
  [...filterDevice.matchAll(/WdfRequestAllocateTimer\s*\(/gu)].length,
  explicitRequestSendCount,
  `${filterDevicePath}: every explicit timed lower request must preallocate its timer`,
);
assert.equal(
  [...filterDevice.matchAll(/WdfRequestGetStatus\s*\(/gu)].length,
  explicitRequestSendCount,
  `${filterDevicePath}: every synchronous explicit send must inspect completion status`,
);
assert.match(
  filterDevice,
  /#define\s+PTP_HID_SYNC_TIMEOUT_MS\s+2000/u,
  `${filterDevicePath}: synchronous HID timeout must remain bounded`,
);

console.log(
  `Driver contracts passed: ${blobs.length} complete PTP HQA blobs, read-only probe, packaging quarantine.`,
);
