// =============================================================================
//  local-history-types.ts — the shape of a local recording (M14).
//
//  Milestone 14 lets the operator record telemetry onto the DEVICE THE
//  DASHBOARD IS OPEN ON — a tablet, a laptop — instead of into the 640 KiB of
//  flash the ESP32 can spare.  That is a genuinely different promise from the
//  firmware's own DataLogger, and the difference is the whole design:
//
//    DataLogger (ESP32)    runs with no browser, survives a closed lid,
//                          is the record of record for an experiment.
//    ClientRecorder (M14)  runs only while this tab is alive, holds far more,
//                          exports easily, and MUST NOT pretend otherwise.
//
//  So every structure here can express a hole.  A tablet that slept, a Wi-Fi
//  drop, a browser that evicted the tab — each becomes a recorded event and a
//  visible gap, never an interpolated line.  An instrument that quietly invents
//  the measurements it missed is worse than one that records nothing.
//
//  No Svelte, no DOM, no IndexedDB in this file: it is the vocabulary the
//  database, the recorder, the exporter and the tests all share.
// =============================================================================

import type { ChannelQuality } from './types';

export const DB_NAME = 'lab-controller-local-history-v1';
export const DB_VERSION = 1;

// --- chunking ----------------------------------------------------------------
//  One IndexedDB transaction per WebSocket frame would be thousands of tiny
//  objects a minute at 50 Hz, a slow index and a tablet writing flash forever.
//  Rows accumulate in RAM and land as columnar blocks.
export const CHUNK_DURATION_MS = 60_000;
export const FLUSH_INTERVAL_MS = 5_000;
export const MAX_PENDING_ROWS = 512;
export const MAX_PENDING_BYTES = 256 * 1024;

/**
 * The hard ceiling on rows waiting to be written.  If IndexedDB cannot keep up
 * the queue does NOT grow: rows past this point are counted as dropped and the
 * session gets a CLIENT_BACKPRESSURE event.  An unbounded queue converts a slow
 * disk into an out-of-memory crash and loses the whole recording instead of a
 * few seconds of it.
 */
export const MAX_QUEUED_ROWS = 4096;

export type LocalSessionState =
  | 'RECORDING' | 'COMPLETE' | 'INTERRUPTED' | 'FULL' | 'ERROR';

export type LocalRateMode = '0.1Hz' | '0.2Hz' | '1Hz' | '5Hz' | 'every';

export type LocalEventType =
  | 'MARK'
  | 'WS_DISCONNECTED'
  | 'WS_RECONNECTED'
  | 'PAGE_SUSPENDED'
  | 'PAGE_RESUMED'
  | 'DEVICE_RESTARTED'
  | 'CONFIG_CHANGED'
  | 'CLIENT_BACKPRESSURE'
  | 'QUOTA_WARNING'
  | 'QUOTA_EXCEEDED';

/** What a channel meant AT THE MOMENT THE RECORDING STARTED.  Renaming a
 *  channel or recalibrating it afterwards must not rewrite finished data. */
export interface LocalChannelDescriptor {
  key: string;
  name: string;
  unit: string;
  quantity: string;
  precision: number;
  calibrationId?: string;
}

export interface LocalSession {
  id: string;
  controllerId: string;
  dashboardKey: string;
  name: string;
  operator: string;
  sample: string;
  notes: string;

  startedClientEpochMs: number;
  endedClientEpochMs?: number;
  startedDeviceMs: number;
  endedDeviceMs?: number;

  firmwareVersion: string;
  configRevision: number;
  state: LocalSessionState;
  stopReason?: string;

  rateMode: LocalRateMode;
  channels: LocalChannelDescriptor[];
  rows: number;
  /** Rows the recorder had to throw away because the database fell behind.
   *  Counted, never hidden. */
  droppedRows: number;
  gaps: number;
  bytes: number;
  /** Set when this session continues an INTERRUPTED one.  A new session, not an
   *  append: nothing can prove the gap between them held no process. */
  parentSessionId?: string;
}

/**
 * A columnar block.  Typed arrays go through structured clone as binary, so a
 * minute of eight channels is a handful of buffers rather than 3600 objects.
 */
export interface LocalChunk {
  sessionId: string;
  sequence: number;
  startEpochMs: number;
  endEpochMs: number;
  rows: number;

  clientEpochMs: Float64Array;
  deviceMs: Float64Array;
  /** Row-major: row0ch0, row0ch1, … row1ch0, … */
  values: Float32Array;
  quality: Uint8Array;
  /** One bit per channel per row.  Distinguishes "no reading" from a real 0 —
   *  without it a disconnected sensor and a balanced bridge look identical. */
  presentMask: Uint32Array;
}

export interface LocalEvent {
  sessionId: string;
  sequence: number;
  clientEpochMs: number;
  deviceMs?: number;
  type: LocalEventType;
  label?: string;
}

export interface LocalSettings {
  controllerId: string;
  dashboardKey: string;
  /** The session that was still RECORDING when the page went away. */
  activeSessionId?: string;
  lastChannels?: string[];
  lastRate?: LocalRateMode;
  /** Soft cap in bytes, 0 for none. */
  softLimitBytes?: number;
}

// --- quality codes -----------------------------------------------------------

/**
 * The stored code for each quality.  Order is the wire format — appending is
 * safe, reordering silently rewrites every recording ever made.
 *
 * NOTE: the M14 design sketch listed `INVALID` and `ERROR`; the firmware's
 * actual vocabulary (types.ts, mirroring ChannelManager) is OUT_OF_RANGE,
 * SATURATED and FAULTED.  The firmware wins: a code table that does not match
 * the instrument would put the wrong word next to every reading in the CSV.
 */
export const QUALITY_ORDER: ChannelQuality[] =
  ['UNKNOWN', 'GOOD', 'STALE', 'OUT_OF_RANGE', 'SATURATED', 'FAULTED'];

export function qualityCode(quality: ChannelQuality | undefined): number {
  const index = quality ? QUALITY_ORDER.indexOf(quality) : 0;
  return index < 0 ? 0 : index;
}

export function qualityName(code: number): ChannelQuality {
  return QUALITY_ORDER[code] ?? 'UNKNOWN';
}

// --- rates -------------------------------------------------------------------

/** Milliseconds between rows, or `null` for "one row per delivered frame". */
export function rateIntervalMs(mode: LocalRateMode): number | null {
  switch (mode) {
    case '0.1Hz': return 10_000;
    case '0.2Hz': return 5_000;
    case '1Hz': return 1_000;
    case '5Hz': return 200;
    case 'every': return null;
  }
}

export function rateLabel(mode: LocalRateMode): string {
  switch (mode) {
    case '0.1Hz': return 'every 10 seconds';
    case '0.2Hz': return 'every 5 seconds';
    case '1Hz': return 'once a second';
    case '5Hz': return '5 times a second';
    // Named honestly: the firmware may coalesce or drop a frame under load, so
    // this is every batch the BROWSER received, not every sensor sample.
    case 'every': return 'every frame received';
  }
}

/**
 * Bytes one stored row costs, per channel: 8 (client epoch) + 8 (device ms)
 * amortised across the row, plus 4 (value) + 1 (quality) + the present bit.
 * Used only for the "18.7 MB per 24 h" estimate the dialog shows before the
 * operator commits a tablet to an overnight run.
 */
export function bytesPerRow(channelCount: number): number {
  return 16 + channelCount * 5 + Math.ceil(channelCount / 8);
}

export function estimateBytes(channelCount: number, mode: LocalRateMode,
                              durationMs: number, framesPerSecond = 5): number {
  const interval = rateIntervalMs(mode);
  const rows = interval === null
    ? (durationMs / 1000) * framesPerSecond
    : durationMs / interval;
  return Math.round(rows * bytesPerRow(channelCount));
}

/** Bytes a chunk occupies — what the session's running total is built from. */
export function chunkBytes(chunk: LocalChunk): number {
  return chunk.clientEpochMs.byteLength + chunk.deviceMs.byteLength
       + chunk.values.byteLength + chunk.quality.byteLength
       + chunk.presentMask.byteLength;
}

/** Words of present-mask per row for a given channel count. */
export function maskWordsPerRow(channelCount: number): number {
  return Math.max(1, Math.ceil(channelCount / 32));
}

export function isPresent(mask: Uint32Array, channelCount: number,
                          row: number, channel: number): boolean {
  const words = maskWordsPerRow(channelCount);
  const word = mask[row * words + (channel >> 5)] ?? 0;
  return (word & (1 << (channel & 31))) !== 0;
}

export function setPresent(mask: Uint32Array, channelCount: number,
                           row: number, channel: number): void {
  const words = maskWordsPerRow(channelCount);
  const index = row * words + (channel >> 5);
  mask[index] = (mask[index] ?? 0) | (1 << (channel & 31));
}

/** Ids are readable on purpose: they appear in exported filenames and in the
 *  delete confirmation, where "which one is this?" must not be a guess. */
export function makeSessionId(startedEpochMs: number, random = Math.random): string {
  const stamp = new Date(startedEpochMs).toISOString()
    .replace(/[-:]/g, '').replace(/\.\d+Z$/, 'Z');
  const suffix = Math.floor(random() * 0x10000).toString(16).padStart(4, '0');
  return `s-${stamp}-${suffix}`;
}
