import {
  Activity,
  Bluetooth,
  Check,
  ChevronRight,
  CircleDot,
  Copy,
  Cpu,
  Download,
  Pause,
  Play,
  Plus,
  Radio,
  RotateCcw,
  ShieldCheck,
  Trash2,
  Upload
} from "lucide-react";
import {
  type ChangeEvent,
  type PointerEvent as ReactPointerEvent,
  useEffect,
  useMemo,
  useRef,
  useState
} from "react";
import { convertReport, getHealth, replayReports, resetSession } from "./api";
import {
  X_MAX,
  Y_MAX,
  defaultContact,
  encodeAppleFrame,
  formatHex,
  rawToDisplayPosition
} from "./codec";
import { presets } from "./presets";
import type {
  ContactInput,
  ConversionResult,
  MagicPadTrace,
  SimulatorFrame
} from "./types";

type SourceId = "manual" | "trace" | (typeof presets)[number]["id"];

const initialFrame: SimulatorFrame = {
  timestampMs: 0,
  button: false,
  contacts: [defaultContact(0)]
};

function ByteView({ hex, empty }: { hex?: string; empty: string }) {
  if (!hex) {
    return <div className="empty-state">{empty}</div>;
  }
  const bytes = hex.match(/.{1,2}/g) ?? [];
  return (
    <div className="byte-view" aria-label={`${bytes.length} 字节报告`}>
      {bytes.map((byte, index) => (
        <span
          className={index === 0 ? "report-byte" : ""}
          key={`${index}-${byte}`}
          title={`Byte ${index}`}
        >
          {byte}
        </span>
      ))}
    </div>
  );
}

function ContactTable({
  result,
  stage
}: {
  result: ConversionResult | null;
  stage: "decoded" | "ptp";
}) {
  const contacts = stage === "decoded"
    ? result?.decoded?.contacts
    : result?.ptp?.contacts;
  if (!contacts?.length) {
    return <div className="empty-state">当前帧没有触点</div>;
  }
  return (
    <div className="contact-table">
      <div className="contact-table-head">
        <span>ID</span><span>X</span><span>Y</span><span>状态</span>
      </div>
      {contacts.map((contact) => {
        if (stage === "decoded" && "absoluteX" in contact) {
          const flags = contact.finger === 6
            ? "Palm"
            : contact.state === 6 ? "Near" : "Touch";
          return (
            <div className="contact-table-row" key={contact.id}>
              <span>{contact.id}</span>
              <span>{contact.absoluteX}</span>
              <span>{contact.absoluteY}</span>
              <span>{flags}</span>
            </div>
          );
        }
        if (stage === "ptp" && "confidence" in contact) {
          return (
            <div className="contact-table-row" key={contact.id}>
              <span>{contact.id}</span>
              <span>{contact.x}</span>
              <span>{contact.y}</span>
              <span className={contact.confidence ? "ok-text" : "warn-text"}>
                C{contact.confidence} · T{contact.tipSwitch}
              </span>
            </div>
          );
        }
        return null;
      })}
    </div>
  );
}

export default function App() {
  const [source, setSource] = useState<SourceId>("manual");
  const [manualFrame, setManualFrame] = useState(initialFrame);
  const [traceFrames, setTraceFrames] = useState<SimulatorFrame[]>([]);
  const [frameIndex, setFrameIndex] = useState(0);
  const [isPlaying, setIsPlaying] = useState(false);
  const [fps, setFps] = useState(24);
  const [result, setResult] = useState<ConversionResult | null>(null);
  const [rawHex, setRawHex] = useState("");
  const [engineOnline, setEngineOnline] = useState(false);
  const [error, setError] = useState("");
  const [copied, setCopied] = useState(false);
  const activePointer = useRef<number | null>(null);
  const activeContact = useRef<number | null>(null);
  const trackpadRef = useRef<HTMLDivElement>(null);
  const fileInputRef = useRef<HTMLInputElement>(null);

  const selectedPreset = presets.find((preset) => preset.id === source);
  const frames = useMemo(() => {
    if (source === "manual") return [manualFrame];
    if (source === "trace") return traceFrames;
    return selectedPreset?.frames ?? [];
  }, [manualFrame, selectedPreset, source, traceFrames]);
  const currentFrame = frames[Math.min(frameIndex, Math.max(0, frames.length - 1))];

  useEffect(() => {
    getHealth()
      .then((health) => setEngineOnline(health.ok))
      .catch((healthError: Error) => {
        setEngineOnline(false);
        setError(healthError.message);
      });
  }, []);

  useEffect(() => {
    if (!currentFrame) return;
    let active = true;
    const timer = window.setTimeout(async () => {
      try {
        setError("");
        let converted: ConversionResult | null = null;
        if (source !== "manual") {
          const reportHexes = frames
            .slice(0, frameIndex + 1)
            .map((frame) => frame.rawReportHex || encodeAppleFrame(frame));
          converted = await replayReports(reportHexes);
        } else {
          const frameHex = currentFrame.rawReportHex || encodeAppleFrame(currentFrame);
          converted = await convertReport(frameHex);
        }
        if (active && converted) {
          setRawHex(currentFrame.rawReportHex || encodeAppleFrame(currentFrame));
          setResult(converted);
        }
      } catch (conversionError) {
        if (active) {
          setError(conversionError instanceof Error
            ? conversionError.message
            : String(conversionError));
        }
      }
    }, source === "manual" ? 35 : 0);
    return () => {
      active = false;
      window.clearTimeout(timer);
    };
  }, [currentFrame, frameIndex, frames, source]);

  useEffect(() => {
    if (!isPlaying || frames.length <= 1) return;
    const timer = window.setInterval(() => {
      setFrameIndex((current) => {
        if (current >= frames.length - 1) {
          setIsPlaying(false);
          return current;
        }
        return current + 1;
      });
    }, 1000 / fps);
    return () => window.clearInterval(timer);
  }, [fps, frames.length, isPlaying]);

  const visualContacts = useMemo(() => {
    if (currentFrame?.contacts.length) return currentFrame.contacts;
    return result?.decoded?.contacts.map((contact) => ({
      id: contact.id,
      ...rawToDisplayPosition(contact.absoluteX, contact.absoluteY),
      pressure: contact.pressure,
      size: contact.size,
      kind: contact.finger === 6
        ? "palm" as const
        : contact.state === 6 ? "near" as const : "finger" as const
    })) ?? [];
  }, [currentFrame, result]);

  function selectSource(nextSource: SourceId) {
    setSource(nextSource);
    setFrameIndex(0);
    setIsPlaying(false);
    setError("");
    void resetSession();
  }

  function updateContact(id: number, patch: Partial<ContactInput>) {
    setManualFrame((frame) => ({
      ...frame,
      timestampMs: frame.timestampMs + 8,
      contacts: frame.contacts.map((contact) =>
        contact.id === id ? { ...contact, ...patch } : contact
      )
    }));
  }

  function updatePointer(event: ReactPointerEvent<HTMLDivElement>) {
    if (
      source !== "manual"
      || activePointer.current !== event.pointerId
      || activeContact.current === null
      || !trackpadRef.current
    ) return;
    const bounds = trackpadRef.current.getBoundingClientRect();
    const x = Math.round(
      Math.max(0, Math.min(1, (event.clientX - bounds.left) / bounds.width))
      * X_MAX
    );
    const y = Math.round(
      Math.max(0, Math.min(1, (event.clientY - bounds.top) / bounds.height))
      * Y_MAX
    );
    updateContact(activeContact.current, { x, y });
  }

  function beginDrag(event: ReactPointerEvent<HTMLDivElement>, id: number) {
    if (source !== "manual") return;
    activePointer.current = event.pointerId;
    activeContact.current = id;
    event.currentTarget.setPointerCapture(event.pointerId);
    updatePointer(event);
  }

  function endDrag(event: ReactPointerEvent<HTMLDivElement>) {
    if (activePointer.current === event.pointerId) {
      activePointer.current = null;
      activeContact.current = null;
      event.currentTarget.releasePointerCapture(event.pointerId);
    }
  }

  function addContact() {
    const used = new Set(manualFrame.contacts.map((contact) => contact.id));
    const id = Array.from({ length: 16 }, (_, index) => index)
      .find((candidate) => !used.has(candidate));
    if (id === undefined || manualFrame.contacts.length >= 5) return;
    setManualFrame((frame) => ({
      ...frame,
      timestampMs: frame.timestampMs + 8,
      contacts: [...frame.contacts, defaultContact(id)]
    }));
  }

  function removeContact(id: number) {
    setManualFrame((frame) => ({
      ...frame,
      timestampMs: frame.timestampMs + 8,
      contacts: frame.contacts.filter((contact) => contact.id !== id)
    }));
  }

  async function resetLab() {
    setIsPlaying(false);
    setFrameIndex(0);
    await resetSession();
    if (source === "manual") {
      setManualFrame(initialFrame);
    }
  }

  async function copyRaw() {
    await navigator.clipboard.writeText(formatHex(rawHex));
    setCopied(true);
    window.setTimeout(() => setCopied(false), 1200);
  }

  async function exportTrace() {
    if (!frames.length) return;
    setError("");
    try {
      await resetSession();
      const exportedFrames = [];
      for (const frame of frames) {
        const reportHex = frame.rawReportHex || encodeAppleFrame(frame);
        const conversion = await convertReport(reportHex);
        exportedFrames.push({
          captureTimestampUs: frame.timestampMs * 1000,
          rawReportHex: reportHex,
          ptpReportHex: conversion.ptpReportHex
        });
      }
      const trace: MagicPadTrace = {
        schema: "magicpad-trace/v1",
        source: "synthetic",
        device: {
          vendorId: "0x004c",
          productId: "0x0324",
          transport: "synthetic"
        },
        frames: exportedFrames
      };
      const blob = new Blob([JSON.stringify(trace, null, 2)], {
        type: "application/json"
      });
      const url = URL.createObjectURL(blob);
      const anchor = document.createElement("a");
      anchor.href = url;
      anchor.download = `magicpad-trace-${source}.json`;
      anchor.click();
      URL.revokeObjectURL(url);
    } catch (exportError) {
      setError(exportError instanceof Error ? exportError.message : String(exportError));
    }
  }

  async function importTrace(event: ChangeEvent<HTMLInputElement>) {
    const file = event.target.files?.[0];
    event.target.value = "";
    if (!file) return;
    try {
      const parsed = JSON.parse(await file.text()) as MagicPadTrace;
      if (
        parsed.schema !== "magicpad-trace/v1"
        || !Array.isArray(parsed.frames)
        || parsed.frames.some((frame) => typeof frame.rawReportHex !== "string")
      ) {
        throw new Error("不是有效的 magicpad-trace/v1 文件");
      }
      setTraceFrames(parsed.frames.map((frame) => ({
        timestampMs: Math.round(frame.captureTimestampUs / 1000),
        button: false,
        contacts: [],
        rawReportHex: frame.rawReportHex
      })));
      selectSource("trace");
    } catch (importError) {
      setError(importError instanceof Error ? importError.message : String(importError));
    }
  }

  const description = source === "manual"
    ? "拖动触点或调整属性，观察同一帧在三个阶段的变化"
    : source === "trace"
      ? `已导入 ${traceFrames.length} 帧 trace`
      : selectedPreset?.description;

  return (
    <div className="app-shell">
      <header className="app-header">
        <div className="brand-mark"><Radio size={22} /></div>
        <div className="brand-copy">
          <h1>MagicPad Driver Lab</h1>
          <p>Apple HID → Windows Precision Touchpad</p>
        </div>
        <div className={`engine-status ${engineOnline ? "online" : "offline"}`}>
          <Activity size={15} />
          <span>{engineOnline ? "原生 C 核心在线" : "转换核心离线"}</span>
        </div>
      </header>

      <main className="workspace">
        <aside className="control-rail" aria-label="模拟器控制">
          <section>
            <div className="section-kicker">INPUT SOURCE</div>
            <label className="field-label" htmlFor="source">输入场景</label>
            <select
              id="source"
              value={source}
              onChange={(event) => selectSource(event.target.value as SourceId)}
            >
              <option value="manual">手动触点</option>
              {presets.map((preset) => (
                <option key={preset.id} value={preset.id}>{preset.label}</option>
              ))}
              {traceFrames.length > 0 && <option value="trace">导入的 Trace</option>}
            </select>
            <p className="section-description">{description}</p>
          </section>

          {source === "manual" && (
            <section className="manual-controls">
              <div className="section-title-row">
                <div>
                  <div className="section-kicker">CONTACTS</div>
                  <strong>{manualFrame.contacts.length} / 5 个触点</strong>
                </div>
                <button
                  className="icon-button"
                  onClick={addContact}
                  disabled={manualFrame.contacts.length >= 5}
                  title="添加触点"
                  aria-label="添加触点"
                >
                  <Plus size={17} />
                </button>
              </div>
              <label className="toggle-row">
                <input
                  type="checkbox"
                  checked={manualFrame.button}
                  onChange={(event) => setManualFrame((frame) => ({
                    ...frame,
                    timestampMs: frame.timestampMs + 8,
                    button: event.target.checked
                  }))}
                />
                <span>物理按键按下</span>
              </label>
              <div className="contact-editors">
                {manualFrame.contacts.map((contact) => (
                  <div className="contact-editor" key={contact.id}>
                    <div className="contact-editor-head">
                      <span className="contact-dot">{contact.id}</span>
                      <select
                        aria-label={`触点 ${contact.id} 类型`}
                        value={contact.kind}
                        onChange={(event) => updateContact(contact.id, {
                          kind: event.target.value as ContactInput["kind"]
                        })}
                      >
                        <option value="finger">Finger</option>
                        <option value="near">Near</option>
                        <option value="palm">Palm</option>
                      </select>
                      <button
                        className="quiet-icon-button"
                        onClick={() => removeContact(contact.id)}
                        title="移除触点"
                        aria-label={`移除触点 ${contact.id}`}
                      >
                        <Trash2 size={15} />
                      </button>
                    </div>
                    <label>
                      <span>Pressure</span><output>{contact.pressure}</output>
                      <input
                        type="range"
                        min="0"
                        max="255"
                        value={contact.pressure}
                        onChange={(event) => updateContact(contact.id, {
                          pressure: Number(event.target.value)
                        })}
                      />
                    </label>
                  </div>
                ))}
              </div>
            </section>
          )}

          <section className="trace-tools">
            <div className="section-kicker">TRACE / SESSION</div>
            <div className="button-grid">
              <button className="secondary-button" onClick={() => fileInputRef.current?.click()}>
                <Upload size={15} /> 导入
              </button>
              <button className="secondary-button" onClick={exportTrace}>
                <Download size={15} /> 导出
              </button>
              <button className="secondary-button wide" onClick={resetLab}>
                <RotateCcw size={15} /> 重置会话
              </button>
            </div>
            <input
              ref={fileInputRef}
              className="visually-hidden"
              type="file"
              accept="application/json,.json"
              onChange={importTrace}
            />
          </section>
        </aside>

        <section className="stage">
          <div className="stage-heading">
            <div>
              <div className="section-kicker">LIVE SURFACE</div>
              <h2>Magic Trackpad 输入面</h2>
            </div>
            <div className="transport-label">
              <Bluetooth size={15} />
              模拟 Bluetooth · PID 0324
            </div>
          </div>

          <div
            className={`trackpad ${source === "manual" ? "interactive" : ""}`}
            ref={trackpadRef}
            onPointerMove={updatePointer}
            onPointerUp={endDrag}
            onPointerCancel={endDrag}
          >
            <div className="trackpad-grid" />
            <div className="trackpad-label">7612 × 5065 logical units</div>
            {visualContacts.map((contact) => {
              const ptpContact = result?.ptp?.contacts.find((item) => item.id === contact.id);
              const rejected = ptpContact?.confidence === 0;
              return (
                <div
                  key={contact.id}
                  className={`touch-point ${contact.kind} ${rejected ? "rejected" : ""}`}
                  style={{
                    left: `${(contact.x / X_MAX) * 100}%`,
                    top: `${(contact.y / Y_MAX) * 100}%`,
                    "--touch-size": `${52 + contact.size * 0.7}px`
                  } as React.CSSProperties}
                  onPointerDown={(event) => beginDrag(event, contact.id)}
                  title={`Contact ${contact.id} · ${contact.kind}`}
                >
                  <span>{contact.id}</span>
                  <small>{rejected ? "C0" : contact.kind}</small>
                </div>
              );
            })}
            {!visualContacts.length && (
              <div className="trackpad-empty">
                <CircleDot size={24} />
                <span>没有活动触点</span>
              </div>
            )}
          </div>

          <div className="timeline">
            <button
              className="play-button"
              disabled={frames.length <= 1}
              onClick={() => {
                if (frameIndex >= frames.length - 1) setFrameIndex(0);
                setIsPlaying((playing) => !playing);
              }}
              aria-label={isPlaying ? "暂停" : "播放"}
              title={isPlaying ? "暂停" : "播放"}
            >
              {isPlaying ? <Pause size={17} /> : <Play size={17} />}
            </button>
            <div className="timeline-main">
              <input
                type="range"
                min="0"
                max={Math.max(0, frames.length - 1)}
                value={Math.min(frameIndex, Math.max(0, frames.length - 1))}
                disabled={frames.length <= 1}
                onChange={(event) => {
                  setIsPlaying(false);
                  setFrameIndex(Number(event.target.value));
                }}
                aria-label="帧时间轴"
              />
              <div className="timeline-meta">
                <span>FRAME {String(frameIndex + 1).padStart(2, "0")} / {String(Math.max(1, frames.length)).padStart(2, "0")}</span>
                <label>
                  <span>FPS</span>
                  <select value={fps} onChange={(event) => setFps(Number(event.target.value))}>
                    <option value="12">12</option>
                    <option value="24">24</option>
                    <option value="60">60</option>
                  </select>
                </label>
              </div>
            </div>
          </div>

          <div className="policy-strip">
            <ShieldCheck size={17} />
            <span>驱动策略</span>
            <strong>Palm rejection</strong>
            <strong>Near finger filtering</strong>
            <strong>Stable 5-contact cap</strong>
          </div>
          {error && <div className="error-banner" role="alert">{error}</div>}
        </section>

        <aside className="pipeline" aria-label="驱动转换流水线">
          <div className="pipeline-title">
            <div>
              <div className="section-kicker">CONVERSION PIPELINE</div>
              <h2>本帧数据</h2>
            </div>
            <Cpu size={20} />
          </div>

          <article className="pipeline-stage">
            <div className="stage-number">01</div>
            <div className="pipeline-stage-content">
              <div className="pipeline-stage-head">
                <div>
                  <strong>Apple HID Report</strong>
                  <span>Report ID 0x31 · {rawHex.length / 2} bytes</span>
                </div>
                <button
                  className="quiet-icon-button"
                  onClick={copyRaw}
                  title="复制原始字节"
                  aria-label="复制原始字节"
                >
                  {copied ? <Check size={15} /> : <Copy size={15} />}
                </button>
              </div>
              <ByteView hex={rawHex} empty="等待 Apple 输入报告" />
            </div>
          </article>

          <ChevronRight className="pipeline-arrow" size={19} />

          <article className="pipeline-stage">
            <div className="stage-number">02</div>
            <div className="pipeline-stage-content">
              <div className="pipeline-stage-head">
                <div>
                  <strong>Decoded MT2 Frame</strong>
                  <span>
                    timestamp {result?.decoded?.timestampMs ?? "—"} ms ·
                    button {result?.decoded?.button ?? "—"}
                  </span>
                </div>
              </div>
              <ContactTable result={result} stage="decoded" />
            </div>
          </article>

          <ChevronRight className="pipeline-arrow" size={19} />

          <article className="pipeline-stage output-stage">
            <div className="stage-number">03</div>
            <div className="pipeline-stage-content">
              <div className="pipeline-stage-head">
                <div>
                  <strong>Windows PTP Report</strong>
                  <span>
                    Report ID 0x05 · {result?.ptp?.contactCount ?? 0} contacts ·
                    scan {result?.ptp?.scanTime ?? "—"}
                  </span>
                </div>
              </div>
              <ContactTable result={result} stage="ptp" />
              <details>
                <summary>查看 50 字节 PTP 报告</summary>
                <ByteView hex={result?.ptpReportHex} empty="等待转换结果" />
              </details>
            </div>
          </article>

          <div className="windows-note">
            <strong>交给 Windows 11</strong>
            <p>驱动只输出原生 PTP 触点；滚动、缩放与桌面切换由系统手势栈识别。</p>
          </div>
        </aside>
      </main>
    </div>
  );
}
