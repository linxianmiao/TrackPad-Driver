import { readFileSync } from "node:fs";
import { pathToFileURL } from "node:url";

// These are prerequisites for a collection-based experiment, not a promise
// that a device mode switch or a future kernel-mode producer will work.
export function analyzeTransport(document) {
  if (document?.schema !== "magicpad-transport-preflight/v1") {
    throw new Error("Unsupported transport preflight schema");
  }
  if (document.scope !== "userModePhysicalBluetoothHidCollection" ||
      !Array.isArray(document.collections)) {
    throw new Error("Invalid preflight scope or collections");
  }
  const findings = [];
  if (document.probeStatus !== "ok") findings.push("incompleteHidProbe");
  if (document.collections.length === 0) findings.push("noPhysicalBluetoothCollection");
  const candidates = document.collections.map((collection) => {
    const blockers = [];
    if (collection.access?.metadata?.Succeeded !== true) blockers.push("metadataOpenFailed");
    if (collection.access?.read?.Succeeded !== true) blockers.push("userReadOpenFailed");
    if (!collection.inputReportIds?.includes(0x31)) blockers.push("input31NotDeclared");
    if (!collection.featureReportIds?.includes(0xf1)) blockers.push("featureF1NotDeclared");
    if (!Number.isInteger(collection.inputReportBytes) || collection.inputReportBytes < 13) {
      blockers.push("inputBufferSmallerThanOneContact");
    }
    for (const [direction, reportId] of [["input", "0x31"], ["feature", "0xf1"]]) {
      const check = collection.parserChecks?.find((item) =>
        item.Direction === direction && item.ReportId === reportId);
      if (check?.Initialized !== true) blockers.push(`${direction}ParserRejected${reportId}`);
    }
    return { collection: collection.collection, blockers };
  });
  const available = candidates.some((candidate) => candidate.blockers.length === 0);
  return {
    schema: "magicpad-transport-analysis/v1",
    status: findings.length > 0 ? "inconclusive"
      : available ? "requiresRuntimeValidation" : "collectionPathBlocked",
    findings,
    candidates,
    modeSwitchValidated: false,
    note: "This result does not validate kernel access, raw transport, mode switching, or VHF.",
  };
}

if (process.argv[1] && import.meta.url === pathToFileURL(process.argv[1]).href) {
  if (process.argv.length !== 3) throw new Error("Usage: node scripts/analyze-transport-preflight.mjs <preflight.json>");
  const document = JSON.parse(readFileSync(process.argv[2], "utf8").replace(/^\uFEFF/u, ""));
  console.log(JSON.stringify(analyzeTransport(document), null, 2));
}
