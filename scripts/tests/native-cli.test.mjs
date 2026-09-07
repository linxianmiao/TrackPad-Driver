import assert from "node:assert/strict";
import { spawn } from "node:child_process";
import { createInterface } from "node:readline";
import { fileURLToPath } from "node:url";
import { test } from "node:test";

test("native CLI replies while stdin remains open, including the fallback request ID", async (t) => {
  const name = process.platform === "win32" ? "amtptp-cli.exe" : "amtptp-cli";
  const child = spawn(fileURLToPath(new URL(`../../core/build/${name}`, import.meta.url)), [], {
    stdio: ["pipe", "pipe", "pipe"], windowsHide: true,
  });
  const lines = createInterface({ input: child.stdout });
  let stderr = "";
  child.stderr.on("data", (data) => { stderr += data; });
  t.after(() => { lines.close(); child.stdin.end(); child.kill(); });

  function request(payload) {
    return new Promise((resolve, reject) => {
      const cleanup = () => {
        clearTimeout(timer);
        lines.off("line", onLine);
        child.off("error", onError);
        child.off("exit", onExit);
      };
      const fail = (error) => { cleanup(); reject(error); };
      const onError = (error) => fail(error);
      const onExit = (code) => fail(new Error(`CLI exited before replying (${code}): ${stderr}`));
      const onLine = (line) => {
        cleanup();
        try { resolve(JSON.parse(line)); } catch (error) { reject(error); }
      };
      const timer = setTimeout(() => fail(new Error(`CLI did not flush its reply: ${stderr}`)), 5000);
      lines.once("line", onLine);
      child.once("error", onError);
      child.once("exit", onExit);
      child.stdin.write(`${JSON.stringify(payload)}\n`, (error) => { if (error) fail(error); });
    });
  }

  assert.deepEqual(await request({ requestId: "reset", command: "reset" }),
    { requestId: "reset", ok: true, reset: true });
  assert.equal(child.stdin.writableEnded, false);
  const response = await request({ reportHex: "31000000" });
  assert.equal(response.ok, true);
  assert.equal(response.requestId, "unknown");
  assert.equal(response.ptpReportHex, "05" + "00".repeat(49));
  assert.equal(child.stdin.writableEnded, false);
});
