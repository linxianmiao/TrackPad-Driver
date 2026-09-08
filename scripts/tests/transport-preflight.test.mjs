import assert from "node:assert/strict";
import { test } from "node:test";
import { analyzeTransport } from "../analyze-transport-preflight.mjs";

function document(collections, probeStatus = "ok") {
  return { schema: "magicpad-transport-preflight/v1", probeStatus,
    scope: "userModePhysicalBluetoothHidCollection", collections };
}
function candidate(overrides = {}) {
  return { collection: "Col01", inputReportIds: [0x31], featureReportIds: [0xf1],
    inputReportBytes: 148, access: { metadata: { Succeeded: true }, read: { Succeeded: true } },
    parserChecks: [
      { Direction: "input", ReportId: "0x31", Initialized: true },
      { Direction: "feature", ReportId: "0xf1", Initialized: true },
    ], ...overrides };
}
test("stock mouse caps block collection-based multitouch", () => {
  const result = analyzeTransport(document([candidate({ inputReportIds: [2],
    featureReportIds: [0x55], inputReportBytes: 8,
    access: { metadata: { Succeeded: true }, read: { Succeeded: false, Win32Error: 5 } },
    parserChecks: [] })]));
  assert.equal(result.status, "collectionPathBlocked");
  assert.ok(result.candidates[0].blockers.includes("input31NotDeclared"));
  assert.ok(result.candidates[0].blockers.includes("featureF1NotDeclared"));
  assert.ok(result.candidates[0].blockers.includes("userReadOpenFailed"));
});
test("missing or partial metadata must not look like an available transport", () => {
  assert.equal(analyzeTransport(document([])).status, "inconclusive");
  assert.equal(analyzeTransport(document([candidate()], "partial")).status, "inconclusive");
});
test("a battery collection cannot supply the input for another collection's feature", () => {
  const result = analyzeTransport(document([
    candidate({ inputReportIds: [2] }), candidate({ collection: "Col02", featureReportIds: [] }),
  ]));
  assert.equal(result.status, "collectionPathBlocked");
});
test("nominal descriptor compatibility never claims mode switching", () => {
  const result = analyzeTransport(document([candidate()]));
  assert.equal(result.status, "requiresRuntimeValidation");
  assert.equal(result.modeSwitchValidated, false);
});
test("invalid or missing read evidence stays blocked", () => {
  const result = analyzeTransport(document([candidate({ access: {}, inputReportBytes: "148" })]));
  assert.equal(result.status, "collectionPathBlocked");
  assert.ok(result.candidates[0].blockers.includes("userReadOpenFailed"));
});
test("USB or unrecognized documents cannot be interpreted as Bluetooth evidence", () => {
  assert.throws(() => analyzeTransport({ schema: "unrelated" }));
  assert.throws(() => analyzeTransport({ ...document([]), scope: "usb" }));
});
