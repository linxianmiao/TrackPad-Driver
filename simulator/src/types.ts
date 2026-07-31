export type ContactKind = "finger" | "near" | "palm";

export interface ContactInput {
  id: number;
  x: number;
  y: number;
  pressure: number;
  size: number;
  kind: ContactKind;
}

export interface SimulatorFrame {
  timestampMs: number;
  button: boolean;
  contacts: ContactInput[];
  rawReportHex?: string;
}

export interface DecodedContact {
  id: number;
  absoluteX: number;
  absoluteY: number;
  finger: number;
  state: number;
  touchMajor: number;
  touchMinor: number;
  size: number;
  pressure: number;
  orientation: number;
}

export interface PtpContact {
  id: number;
  x: number;
  y: number;
  confidence: number;
  tipSwitch: number;
}

export interface ConversionResult {
  requestId: string;
  ok: boolean;
  error?: string;
  decoded?: {
    timestampMs: number;
    button: number;
    contacts: DecodedContact[];
  };
  ptp?: {
    scanTime: number;
    button: number;
    contactCount: number;
    contacts: PtpContact[];
  };
  ptpReportHex?: string;
}

export interface TraceFrame {
  captureTimestampUs: number;
  rawReportHex: string;
  ptpReportHex?: string;
}

export interface MagicPadTrace {
  schema: "magicpad-trace/v1";
  source: "captured" | "synthetic";
  device: {
    vendorId: string;
    productId: string;
    transport: "bluetooth" | "usb" | "synthetic";
  };
  frames: TraceFrame[];
}
