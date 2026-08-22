// =============================================================================
//  log-offload/crc32.ts — the number that authorises a deletion (M15).
//
//  The controller erases a CSV from its flash once this browser says it holds
//  the same bytes.  That is only safe if both ends compute the same checksum
//  over the same range, so this is deliberately the ordinary CRC-32 (zlib/PNG:
//  polynomial 0xEDB88320, reflected, init and final xor 0xFFFFFFFF) and matches
//  firmware/src/core/Crc32.h byte for byte.
//
//  The two implementations differ in shape — a 256-entry table here, a nibble
//  table on the microcontroller — and are held together by shared test vectors
//  rather than by good intentions.  Two checksums that agree only by assertion
//  are how a corrupted transfer ends with a deleted original.
//
//  THE RANGE MATTERS AS MUCH AS THE ALGORITHM.  The checksum covers the data
//  rows only: everything after the column line and before the footer.  The
//  header is excluded because it is written before the rows exist, and the
//  footer because it is where the checksum itself is written — a number cannot
//  be part of what it is computed over.
// =============================================================================

const TABLE = (() => {
  const table = new Uint32Array(256);
  for (let i = 0; i < 256; ++i) {
    let c = i;
    for (let k = 0; k < 8; ++k) c = c & 1 ? 0xEDB88320 ^ (c >>> 1) : c >>> 1;
    table[i] = c >>> 0;
  }
  return table;
})();

/** Fold more bytes in; `seed` continues a previous run. */
export function crc32(bytes: Uint8Array, seed = 0): number {
  let crc = (seed ^ 0xFFFFFFFF) >>> 0;
  for (let i = 0; i < bytes.length; ++i) {
    crc = (TABLE[(crc ^ bytes[i]!) & 0xFF]! ^ (crc >>> 8)) >>> 0;
  }
  return (crc ^ 0xFFFFFFFF) >>> 0;
}

export function crc32Hex(bytes: Uint8Array): string {
  return crc32(bytes).toString(16).padStart(8, '0');
}

const COLUMN_MARKER = 'quality_mask\n';
const FOOTER_MARKER = '\n# segment_complete';

/**
 * The payload of a downloaded segment: the rows, and only the rows.
 *
 * Returns null when the file does not look like a finished segment — a missing
 * column line or a missing footer means a truncated download, and the caller
 * must treat that as a failed transfer rather than checksum whatever arrived.
 */
export function segmentPayload(file: Uint8Array): Uint8Array | null {
  const text = new TextDecoder('utf-8', { fatal: false }).decode(file);
  const columns = text.indexOf(COLUMN_MARKER);
  if (columns < 0) return null;
  const footer = text.indexOf(FOOTER_MARKER, columns);
  if (footer < 0) return null;
  // Byte offsets, not character offsets: the header carries operator and sample
  // names, which are routinely not ASCII.
  const encoder = new TextEncoder();
  const start = encoder.encode(text.slice(0, columns + COLUMN_MARKER.length)).length;
  const end = encoder.encode(text.slice(0, footer + 1)).length;
  if (end < start) return null;
  return file.subarray(start, end);
}

/** What the segment's own footer claims, for cross-checking against the index
 *  the controller served.  Two independent statements of the same number. */
export function footerChecksum(file: Uint8Array): string | null {
  const text = new TextDecoder('utf-8', { fatal: false }).decode(file);
  const match = /# payload_crc32: ([0-9a-f]{8})/.exec(text);
  return match ? match[1]! : null;
}

export interface VerificationResult {
  ok: boolean;
  reason: string;
  computed: string;
}

/**
 * Does this file deserve an acknowledgement?
 *
 * Size first, then checksum, then the footer's own claim.  All three, because
 * they fail differently: a short read matches no checksum, a corrupted byte
 * passes the size check, and a file assembled from two different downloads can
 * pass both while its footer says something else entirely.
 */
export function verifySegment(file: Uint8Array, expectedBytes: number,
                              expectedCrc: string): VerificationResult {
  if (file.length !== expectedBytes) {
    return {
      ok: false,
      computed: '',
      reason: `expected ${expectedBytes} bytes, received ${file.length}`,
    };
  }
  const payload = segmentPayload(file);
  if (payload === null) {
    return { ok: false, computed: '', reason: 'the file has no complete footer' };
  }
  const computed = crc32Hex(payload);
  if (computed !== expectedCrc.toLowerCase()) {
    return {
      ok: false,
      computed,
      reason: `checksum ${computed} does not match ${expectedCrc}`,
    };
  }
  const claimed = footerChecksum(file);
  if (claimed !== null && claimed !== computed) {
    return {
      ok: false,
      computed,
      reason: `the footer claims ${claimed} but the rows compute to ${computed}`,
    };
  }
  return { ok: true, computed, reason: '' };
}
