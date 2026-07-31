import type { ContactInput, SimulatorFrame } from "./types";

export const X_MAX = 7612;
export const Y_MAX = 5065;
const X_MIN_RAW = -3678;
const Y_MIN_RAW = -2479;

function encodeSigned13(value: number): number {
  return Math.round(value) & 0x1fff;
}

function byteToHex(value: number): string {
  return value.toString(16).padStart(2, "0");
}

export function encodeAppleFrame(frame: SimulatorFrame): string {
  const contacts = frame.contacts.slice(0, 16);
  const timestamp = Math.max(0, Math.round(frame.timestampMs)) & 0x1fffff;
  const bytes = new Uint8Array(4 + contacts.length * 9);
  bytes[0] = 0x31;
  bytes[1] = (frame.button ? 1 : 0) | ((timestamp & 0x1f) << 3);
  bytes[2] = (timestamp >>> 5) & 0xff;
  bytes[3] = (timestamp >>> 13) & 0xff;

  contacts.forEach((contact, index) => {
    const absoluteX = encodeSigned13(contact.x + X_MIN_RAW);
    const absoluteY = encodeSigned13(-(contact.y + Y_MIN_RAW));
    const finger = contact.kind === "palm" ? 6 : 2;
    const state = contact.kind === "near" ? 6 : 4;
    const packed = (
      absoluteX
      | (absoluteY << 13)
      | ((finger & 0x07) << 26)
      | ((state & 0x07) << 29)
    ) >>> 0;
    const offset = 4 + index * 9;
    bytes[offset] = packed & 0xff;
    bytes[offset + 1] = (packed >>> 8) & 0xff;
    bytes[offset + 2] = (packed >>> 16) & 0xff;
    bytes[offset + 3] = (packed >>> 24) & 0xff;
    bytes[offset + 4] = Math.max(1, Math.min(255, contact.size));
    bytes[offset + 5] = Math.max(1, Math.min(255, contact.size - 2));
    bytes[offset + 6] = Math.max(0, Math.min(255, contact.size));
    bytes[offset + 7] = Math.max(0, Math.min(255, contact.pressure));
    bytes[offset + 8] = contact.id & 0x0f;
  });

  return Array.from(bytes, byteToHex).join("");
}

export function defaultContact(id: number): ContactInput {
  return {
    id,
    x: Math.round(X_MAX * (0.32 + id * 0.12)),
    y: Math.round(Y_MAX * 0.48),
    pressure: 42,
    size: 18,
    kind: "finger"
  };
}

export function rawToDisplayPosition(absoluteX: number, absoluteY: number) {
  return {
    x: Math.max(0, Math.min(X_MAX, absoluteX - X_MIN_RAW)),
    y: Math.max(0, Math.min(Y_MAX, -absoluteY - Y_MIN_RAW))
  };
}

export function formatHex(hex: string, group = 2): string {
  const chunks = hex.match(new RegExp(`.{1,${group * 2}}`, "g"));
  return chunks?.join(" ") ?? "";
}
