// =============================================================================
//  format.ts — how numbers are shown.
//
//  Two rules that matter more than they look:
//    * a measurement always renders with its declared precision, so the digits
//      do not change width as the value moves and the eye can read a column;
//    * a value that is not GOOD is never shown as if it were.  Stale, faulted
//      and out-of-range each look different, because hiding an anomaly hides a
//      fault.
// =============================================================================

import type { ApiError, ChannelQuality } from './types';

export function formatValue(
  value: number | undefined,
  precision = 2,
  // Deliberately NOT defaulted to 'GOOD': a call site that does not know the
  // quality must not thereby assert that the number is fresh.
  quality?: ChannelQuality,
): string {
  if (value === undefined || quality === 'UNKNOWN') return '—';
  if (!Number.isFinite(value)) return 'n/a';
  return value.toFixed(precision);
}

export function qualityClass(quality: ChannelQuality | undefined): string {
  switch (quality) {
    case 'GOOD': return 'q-good';
    case 'STALE': return 'q-stale';
    case 'OUT_OF_RANGE': return 'q-range';
    case 'SATURATED': return 'q-range';
    case 'FAULTED': return 'q-fault';
    default: return 'q-unknown';
  }
}

export function qualityLabel(quality: ChannelQuality | undefined): string {
  switch (quality) {
    case 'STALE': return 'stale';
    case 'OUT_OF_RANGE': return 'out of range';
    case 'SATURATED': return 'saturated';
    case 'FAULTED': return 'faulted';
    case 'UNKNOWN': return 'no data';
    default: return '';
  }
}

export function stateClass(state: string | undefined): string {
  switch (state) {
    case 'RUNNING': return 's-ok';
    case 'WARNING': return 's-warn';
    case 'ERROR': return 's-error';
    case 'INITIALIZING': return 's-busy';
    default: return 's-idle';
  }
}

export function formatBytes(bytes: number | undefined): string {
  if (bytes === undefined) return '—';
  if (bytes < 1024) return `${bytes} B`;
  if (bytes < 1024 * 1024) return `${(bytes / 1024).toFixed(1)} KiB`;
  return `${(bytes / 1024 / 1024).toFixed(2)} MiB`;
}

export function formatDuration(ms: number | undefined): string {
  if (ms === undefined) return '—';
  const seconds = Math.floor(ms / 1000);
  const days = Math.floor(seconds / 86400);
  const hours = Math.floor((seconds % 86400) / 3600);
  const minutes = Math.floor((seconds % 3600) / 60);
  if (days > 0) return `${days} d ${hours} h`;
  if (hours > 0) return `${hours} h ${minutes} m`;
  if (minutes > 0) return `${minutes} m ${seconds % 60} s`;
  return `${seconds} s`;
}

export function formatInterval(microseconds: number | undefined): string {
  if (!microseconds) return '—';
  const hz = 1_000_000 / microseconds;
  if (hz >= 1) return `${hz.toFixed(hz >= 10 ? 0 : 1)} Hz`;
  return `${(microseconds / 1_000_000).toFixed(1)} s`;
}

/** "0x76" from 118, for I²C addresses. */
export function hex(value: number, width = 2): string {
  return `0x${value.toString(16).toUpperCase().padStart(width, '0')}`;
}

/**
 * One readable sentence out of the error envelope.
 *
 * `message` is the human wording, `detail` is the specific fact ("used by I2C0
 * SDA"), and either can be absent.  Showing only `detail` — which is what the
 * wizard used to do — turns "GPIO21 is already in use — used by I2C0 SDA" into
 * the bare fragment "used by I2C0 SDA".
 */
export function errorSentence(error: ApiError): string {
  const human = error.message && error.message !== error.code ? error.message : '';
  const detail = error.detail && error.detail !== human ? error.detail : '';
  if (human && detail) return `${human} — ${detail}`;
  return human || detail || error.code;
}
