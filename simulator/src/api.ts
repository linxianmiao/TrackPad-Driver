import type { ConversionResult } from "./types";

async function request<T>(path: string, body?: unknown): Promise<T> {
  const response = await fetch(path, {
    method: body === undefined ? "GET" : "POST",
    headers: body === undefined ? undefined : { "Content-Type": "application/json" },
    body: body === undefined ? undefined : JSON.stringify(body)
  });
  const result = await response.json();
  if (!response.ok) {
    throw new Error(result.error || `请求失败：${response.status}`);
  }
  return result as T;
}

export function convertReport(reportHex: string): Promise<ConversionResult> {
  return request<ConversionResult>("/api/convert", { reportHex });
}

export function replayReports(reportHexes: string[]): Promise<ConversionResult> {
  return request<ConversionResult>("/api/replay", { reportHexes });
}

export function resetSession(): Promise<{ ok: boolean }> {
  return request<{ ok: boolean }>("/api/reset", {});
}

export function getHealth(): Promise<{ ok: boolean; engine: string }> {
  return request<{ ok: boolean; engine: string }>("/api/health");
}
