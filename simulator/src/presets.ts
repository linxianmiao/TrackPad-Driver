import { X_MAX, Y_MAX } from "./codec";
import type { ContactInput, SimulatorFrame } from "./types";

export interface Preset {
  id: string;
  label: string;
  description: string;
  frames: SimulatorFrame[];
}

function contact(
  id: number,
  x: number,
  y: number,
  kind: ContactInput["kind"] = "finger"
): ContactInput {
  return { id, x, y, pressure: 48, size: 19, kind };
}

function sequence(
  count: number,
  factory: (progress: number) => ContactInput[],
  button = false
): SimulatorFrame[] {
  return Array.from({ length: count }, (_, index) => ({
    timestampMs: index * 8,
    button,
    contacts: factory(index / Math.max(1, count - 1))
  }));
}

export const presets: Preset[] = [
  {
    id: "move",
    label: "单指移动",
    description: "一个稳定 Contact ID 横向移动",
    frames: sequence(28, (t) => [
      contact(0, X_MAX * (0.16 + 0.68 * t), Y_MAX * (0.47 + 0.05 * Math.sin(t * Math.PI)))
    ])
  },
  {
    id: "scroll",
    label: "双指滚动",
    description: "两个触点同步纵向移动，由 Windows 识别滚动",
    frames: sequence(28, (t) => [
      contact(0, X_MAX * 0.43, Y_MAX * (0.7 - 0.42 * t)),
      contact(1, X_MAX * 0.57, Y_MAX * (0.7 - 0.42 * t))
    ])
  },
  {
    id: "pinch",
    label: "双指捏合",
    description: "两个触点距离收缩，由 Windows 识别缩放",
    frames: sequence(28, (t) => [
      contact(0, X_MAX * (0.18 + 0.23 * t), Y_MAX * 0.5),
      contact(1, X_MAX * (0.82 - 0.23 * t), Y_MAX * 0.5)
    ])
  },
  {
    id: "swipe",
    label: "三指横扫",
    description: "三个触点保持间距横移",
    frames: sequence(30, (t) => [0, 1, 2].map((id) =>
      contact(id, X_MAX * (0.12 + id * 0.11 + 0.48 * t), Y_MAX * (0.43 + id * 0.08))
    ))
  },
  {
    id: "palm",
    label: "掌压拒绝",
    description: "Palm 类型保留触点但清除 Confidence",
    frames: sequence(18, (t) => [
      contact(0, X_MAX * (0.3 + 0.25 * t), Y_MAX * 0.42),
      contact(1, X_MAX * 0.68, Y_MAX * 0.57, "palm")
    ])
  }
];
