// Plan A — Foxglove User Script example (ego_motion).
// Copy into Foxglove Studio: Panel → User Script → paste / adapt.
// Input:  /thyme/ego_motion  (JSON envelope from viz_bridge)
// Output: /thyme/decoded/ego_motion  (fields for Plot / Table)
//
// Replace offset/layout with your contract doc or AI-generated layout.
// This dummy stub treats payload as FrameHeader (32 B) + first float at byte 32.

type Envelope = {
  mono_ns: number;
  source_sequence: number;
  payload_b64: string;
};

type DecodedEgoMotion = {
  mono_ns: number;
  source_sequence: number;
  // example scalar — adjust offset to real EgoMotion layout
  example_scalar: number;
};

function b64ToBytes(b64: string): Uint8Array {
  const bin = atob(b64);
  const out = new Uint8Array(bin.length);
  for (let i = 0; i < bin.length; i++) out[i] = bin.charCodeAt(i);
  return out;
}

function readF32LE(bytes: Uint8Array, offset: number): number {
  const view = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
  return view.getFloat32(offset, true);
}

export const inputs = ["/thyme/ego_motion"];
export const output = "/thyme/decoded/ego_motion";

export default function script(event: {
  topic: string;
  message: Envelope;
}): DecodedEgoMotion {
  const msg = event.message;
  const bytes = b64ToBytes(msg.payload_b64);
  const kFrameHeaderBytes = 32;
  const example_scalar =
    bytes.length > kFrameHeaderBytes + 4
      ? readF32LE(bytes, kFrameHeaderBytes)
      : NaN;
  return {
    mono_ns: msg.mono_ns,
    source_sequence: msg.source_sequence,
    example_scalar,
  };
}
