import { spawn, spawnSync } from "node:child_process";
import { createReadStream, existsSync } from "node:fs";
import { access } from "node:fs/promises";
import { createServer } from "node:http";
import { dirname, extname, join, normalize, resolve } from "node:path";
import { fileURLToPath } from "node:url";
import { createInterface } from "node:readline";

const simulatorDir = dirname(fileURLToPath(import.meta.url));
const repositoryDir = resolve(simulatorDir, "..");
const coreDir = join(repositoryDir, "core");
const cliPath = join(coreDir, "build", "amtptp-cli");
const distDir = join(simulatorDir, "dist");
const isDevelopment = process.argv.includes("--dev");
const port = Number(process.env.MAGICPAD_SIMULATOR_PORT || 4173);
const host = process.env.MAGICPAD_SIMULATOR_HOST || "127.0.0.1";

const build = spawnSync("make", ["-C", coreDir, "all"], {
  stdio: "inherit"
});
if (build.status !== 0 || !existsSync(cliPath)) {
  throw new Error("无法构建共享转换核心 core/build/amtptp-cli");
}

const bridge = spawn(cliPath, [], {
  cwd: repositoryDir,
  stdio: ["pipe", "pipe", "inherit"]
});
const pending = new Map();
let sequence = 0;
let operationTail = Promise.resolve();

createInterface({ input: bridge.stdout }).on("line", (line) => {
  let message;
  try {
    message = JSON.parse(line);
  } catch {
    return;
  }
  const waiter = pending.get(message.requestId);
  if (waiter) {
    pending.delete(message.requestId);
    waiter.resolve(message);
  }
});

bridge.on("exit", (code) => {
  for (const waiter of pending.values()) {
    waiter.reject(new Error(`转换核心已退出，状态码 ${code}`));
  }
  pending.clear();
});

function callBridge(payload) {
  const requestId = `web-${++sequence}`;
  return new Promise((resolvePromise, rejectPromise) => {
    const timer = setTimeout(() => {
      pending.delete(requestId);
      rejectPromise(new Error("转换核心响应超时"));
    }, 3000);
    pending.set(requestId, {
      resolve: (result) => {
        clearTimeout(timer);
        resolvePromise(result);
      },
      reject: (error) => {
        clearTimeout(timer);
        rejectPromise(error);
      }
    });
    bridge.stdin.write(`${JSON.stringify({ ...payload, requestId })}\n`);
  });
}

function enqueueOperation(operation) {
  const current = operationTail.then(operation, operation);
  operationTail = current.catch(() => undefined);
  return current;
}

async function readJsonBody(request) {
  const chunks = [];
  let length = 0;
  for await (const chunk of request) {
    length += chunk.length;
    if (length > 64 * 1024) {
      throw new Error("请求体过大");
    }
    chunks.push(chunk);
  }
  return JSON.parse(Buffer.concat(chunks).toString("utf8") || "{}");
}

function sendJson(response, status, body) {
  response.writeHead(status, {
    "Content-Type": "application/json; charset=utf-8",
    "Cache-Control": "no-store"
  });
  response.end(JSON.stringify(body));
}

const mimeTypes = {
  ".css": "text/css; charset=utf-8",
  ".html": "text/html; charset=utf-8",
  ".js": "text/javascript; charset=utf-8",
  ".json": "application/json; charset=utf-8",
  ".map": "application/json; charset=utf-8",
  ".svg": "image/svg+xml"
};

async function serveStatic(request, response) {
  const url = new URL(request.url, `http://${request.headers.host}`);
  const requested = url.pathname === "/" ? "index.html" : url.pathname.slice(1);
  const safePath = normalize(requested).replace(/^(\.\.(\/|\\|$))+/, "");
  let filePath = join(distDir, safePath);
  try {
    await access(filePath);
  } catch {
    filePath = join(distDir, "index.html");
  }
  response.writeHead(200, {
    "Content-Type": mimeTypes[extname(filePath)] || "application/octet-stream",
    "Cache-Control": filePath.endsWith("index.html")
      ? "no-cache"
      : "public, max-age=31536000, immutable"
  });
  createReadStream(filePath).pipe(response);
}

let vite;
if (isDevelopment) {
  const { createServer: createViteServer } = await import("vite");
  vite = await createViteServer({
    root: simulatorDir,
    server: { middlewareMode: true },
    appType: "spa"
  });
} else if (!existsSync(join(distDir, "index.html"))) {
  throw new Error("请先运行 npm run build");
}

const server = createServer(async (request, response) => {
  try {
    const url = new URL(request.url, `http://${request.headers.host}`);
    if (request.method === "GET" && url.pathname === "/api/health") {
      sendJson(response, 200, {
        ok: true,
        engine: "amtptp native C core",
        reportBytes: 50
      });
      return;
    }
    if (request.method === "POST" && url.pathname === "/api/convert") {
      const body = await readJsonBody(request);
      if (typeof body.reportHex !== "string") {
        sendJson(response, 400, { ok: false, error: "缺少 reportHex" });
        return;
      }
      const result = await enqueueOperation(
        () => callBridge({ reportHex: body.reportHex })
      );
      sendJson(response, result.ok ? 200 : 422, result);
      return;
    }
    if (request.method === "POST" && url.pathname === "/api/replay") {
      const body = await readJsonBody(request);
      if (
        !Array.isArray(body.reportHexes)
        || body.reportHexes.length === 0
        || body.reportHexes.length > 512
        || body.reportHexes.some((value) => typeof value !== "string")
      ) {
        sendJson(response, 400, {
          ok: false,
          error: "reportHexes 必须包含 1–512 个报告"
        });
        return;
      }
      const result = await enqueueOperation(async () => {
        await callBridge({ command: "reset" });
        let converted;
        for (const reportHex of body.reportHexes) {
          converted = await callBridge({ reportHex });
          if (!converted.ok) break;
        }
        return converted;
      });
      sendJson(response, result?.ok ? 200 : 422, result);
      return;
    }
    if (request.method === "POST" && url.pathname === "/api/reset") {
      const result = await enqueueOperation(
        () => callBridge({ command: "reset" })
      );
      sendJson(response, 200, result);
      return;
    }
    if (vite) {
      vite.middlewares(request, response, (error) => {
        if (error) {
          sendJson(response, 500, { ok: false, error: error.message });
        }
      });
      return;
    }
    await serveStatic(request, response);
  } catch (error) {
    sendJson(response, 500, {
      ok: false,
      error: error instanceof Error ? error.message : String(error)
    });
  }
});

server.listen(port, host, () => {
  console.log(`MagicPad Driver Lab: http://${host}:${port}`);
});

function shutdown() {
  bridge.kill();
  server.close(() => process.exit(0));
}

process.on("SIGINT", shutdown);
process.on("SIGTERM", shutdown);
