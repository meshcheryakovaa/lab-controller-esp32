// =============================================================================
//  local-export.ts — getting a local recording back out (M14).
//
//  A week of sixteen channels is gigabytes.  Every function here therefore
//  STREAMS: chunks are read one at a time and turned into text or into bucketed
//  extremes as they pass.  Building the CSV in a JavaScript string first would
//  work beautifully on the developer's laptop with a five-minute test recording
//  and fail on the tablet holding the run that mattered.
//
//  The same rule covers reading for the screen: `readSeries` keeps one bucket
//  per output pixel, not one point per row, so opening a three-day session
//  costs the same as opening a three-minute one.
// =============================================================================

import {
  isPresent, qualityName,
  type LocalChunk, type LocalEvent, type LocalSession,
} from './local-history-types';
import type { LocalHistoryDb } from './local-history-db';

// -----------------------------------------------------------------------------
//  CSV
// -----------------------------------------------------------------------------

/** The reproducibility header, matching what the ESP32's own CSV carries (§48)
 *  plus the one thing that file cannot say: this came from a browser. */
export function csvPreamble(session: LocalSession): string[] {
  return [
    '# source: client IndexedDB',
    `# controller_id: ${session.controllerId}`,
    `# firmware: ${session.firmwareVersion}`,
    `# dashboard: ${session.dashboardKey}`,
    `# session_id: ${session.id}`,
    `# name: ${session.name}`,
    `# operator: ${session.operator}`,
    `# sample: ${session.sample}`,
    `# config_revision: ${session.configRevision}`,
    `# recording_rate: ${session.rateMode}`,
    `# started: ${new Date(session.startedClientEpochMs).toISOString()}`,
    session.endedClientEpochMs
      ? `# ended: ${new Date(session.endedClientEpochMs).toISOString()}`
      : '# ended: (not closed)',
    `# rows: ${session.rows}`,
    `# dropped_rows: ${session.droppedRows}`,
    `# gaps: ${session.gaps}`,
    `# state: ${session.state}`,
    session.stopReason ? `# stop_reason: ${session.stopReason}` : '# stop_reason: -',
    session.parentSessionId ? `# continues: ${session.parentSessionId}` : '# continues: -',
    '# NOTE: gaps are real. Missing rows are periods this device was not'
      + ' recording; they are not interpolated.',
  ];
}

export function csvHeader(session: LocalSession): string {
  const columns = ['client_epoch_ms', 'client_iso', 'device_ms'];
  for (const channel of session.channels) {
    columns.push(`${channel.key}[${channel.unit}]`, `${channel.key}.quality`);
  }
  return columns.join(',');
}

function formatValue(value: number, precision: number): string {
  if (!Number.isFinite(value)) return '';
  return precision > 0 ? value.toFixed(precision) : String(value);
}

/**
 * The rows of one session, as text, one chunk of the database at a time.
 *
 * An async generator rather than an array: the caller pipes it straight into a
 * stream, so peak memory is one chunk however long the recording ran.
 */
export async function* csvRows(db: LocalHistoryDb, session: LocalSession,
                               from?: number, to?: number): AsyncGenerator<string> {
  const channelCount = session.channels.length;
  const chunks: LocalChunk[] = [];
  // eachChunk hands blocks over one at a time; collect only the current one.
  await db.eachChunk(session.id, (chunk) => { chunks.push(chunk); }, from, to);
  for (const chunk of chunks) {
    // One string per chunk, not per session: 60 s of rows is a few KB.
    const lines: string[] = [];
    for (let row = 0; row < chunk.rows; ++row) {
      const clientEpochMs = chunk.clientEpochMs[row]!;
      if (from !== undefined && clientEpochMs < from) continue;
      if (to !== undefined && clientEpochMs > to) continue;
      const cells: string[] = [
        String(clientEpochMs),
        new Date(clientEpochMs).toISOString(),
        String(chunk.deviceMs[row]!),
      ];
      for (let c = 0; c < channelCount; ++c) {
        const index = row * channelCount + c;
        // An absent reading is an empty cell, never a zero.  A balanced bridge
        // really does read 0.000 g, and the two must not look alike.
        if (!isPresent(chunk.presentMask, channelCount, row, c)) {
          cells.push('', qualityName(chunk.quality[index] ?? 0));
          continue;
        }
        cells.push(formatValue(chunk.values[index]!, session.channels[c]!.precision),
                   qualityName(chunk.quality[index] ?? 0));
      }
      lines.push(cells.join(','));
    }
    if (lines.length > 0) yield `${lines.join('\n')}\n`;
  }
}

export async function* csvDocument(db: LocalHistoryDb, session: LocalSession,
                                   from?: number, to?: number): AsyncGenerator<string> {
  yield `${csvPreamble(session).join('\n')}\n`;
  yield `${csvHeader(session)}\n`;
  yield* csvRows(db, session, from, to);
}

export function eventsCsv(session: LocalSession, events: LocalEvent[]): string {
  const lines = ['sequence,client_epoch_ms,client_iso,device_ms,type,label'];
  for (const event of events) {
    const label = (event.label ?? '').replace(/"/g, '""');
    lines.push([
      String(event.sequence),
      String(event.clientEpochMs),
      new Date(event.clientEpochMs).toISOString(),
      event.deviceMs === undefined ? '' : String(event.deviceMs),
      event.type,
      label.includes(',') ? `"${label}"` : label,
    ].join(','));
  }
  lines.unshift(`# session_id: ${session.id}`);
  return `${lines.join('\n')}\n`;
}

export function sessionJson(session: LocalSession, events: LocalEvent[]): string {
  return `${JSON.stringify({ session, events }, null, 2)}\n`;
}

// -----------------------------------------------------------------------------
//  Streams
// -----------------------------------------------------------------------------

/** Text pieces to bytes, without ever concatenating the whole document. */
export function toByteStream(pieces: AsyncIterable<string>): ReadableStream<Uint8Array> {
  const encoder = new TextEncoder();
  const iterator = pieces[Symbol.asyncIterator]();
  return new ReadableStream<Uint8Array>({
    async pull(controller) {
      const next = await iterator.next();
      if (next.done) { controller.close(); return; }
      controller.enqueue(encoder.encode(next.value));
    },
  });
}

// -----------------------------------------------------------------------------
//  ZIP (stored, streaming)
// -----------------------------------------------------------------------------

const CRC_TABLE = (() => {
  const table = new Uint32Array(256);
  for (let i = 0; i < 256; ++i) {
    let c = i;
    for (let k = 0; k < 8; ++k) c = c & 1 ? 0xEDB88320 ^ (c >>> 1) : c >>> 1;
    table[i] = c >>> 0;
  }
  return table;
})();

export function crc32(bytes: Uint8Array, seed = 0): number {
  let crc = (seed ^ 0xFFFFFFFF) >>> 0;
  for (let i = 0; i < bytes.length; ++i) {
    crc = (CRC_TABLE[(crc ^ bytes[i]!) & 0xFF]! ^ (crc >>> 8)) >>> 0;
  }
  return (crc ^ 0xFFFFFFFF) >>> 0;
}

interface ZipEntry {
  name: string;
  content: AsyncIterable<string>;
}

function u32(value: number): Uint8Array {
  const out = new Uint8Array(4);
  new DataView(out.buffer).setUint32(0, value >>> 0, true);
  return out;
}

function u16(value: number): Uint8Array {
  const out = new Uint8Array(2);
  new DataView(out.buffer).setUint16(0, value & 0xFFFF, true);
  return out;
}

function concat(parts: Uint8Array[]): Uint8Array {
  const total = parts.reduce((n, p) => n + p.length, 0);
  const out = new Uint8Array(total);
  let at = 0;
  for (const part of parts) { out.set(part, at); at += part.length; }
  return out;
}

/**
 * A ZIP written as it is produced: entries are STORED (no compression) and use
 * data descriptors, so nothing needs to know a file's size before writing it.
 * That is what lets `data.csv` be gigabytes without ever existing in memory.
 *
 * The bytes are plain ZIP — the archive opens in Explorer, Finder and `unzip`.
 */
export function zipStream(entries: ZipEntry[]): ReadableStream<Uint8Array> {
  const encoder = new TextEncoder();
  const central: Uint8Array[] = [];
  let offset = 0;
  let index = 0;

  return new ReadableStream<Uint8Array>({
    async pull(controller) {
      if (index >= entries.length) {
        const dir = concat(central);
        controller.enqueue(dir);
        controller.enqueue(concat([
          u32(0x06054b50), u16(0), u16(0),
          u16(entries.length), u16(entries.length),
          u32(dir.length), u32(offset), u16(0),
        ]));
        controller.close();
        return;
      }
      const entry = entries[index++]!;
      const name = encoder.encode(entry.name);
      const localOffset = offset;
      // Flag bit 3: sizes and CRC follow the data in a descriptor.
      const header = concat([
        u32(0x04034b50), u16(20), u16(0x0008), u16(0),
        u16(0), u16(0), u32(0), u32(0), u32(0),
        u16(name.length), u16(0), name,
      ]);
      controller.enqueue(header);
      offset += header.length;

      let crc = 0;
      let size = 0;
      for await (const piece of entry.content) {
        const bytes = encoder.encode(piece);
        crc = crc32(bytes, crc);
        size += bytes.length;
        controller.enqueue(bytes);
        offset += bytes.length;
      }
      const descriptor = concat([u32(0x08074b50), u32(crc), u32(size), u32(size)]);
      controller.enqueue(descriptor);
      offset += descriptor.length;

      central.push(concat([
        u32(0x02014b50), u16(20), u16(20), u16(0x0008), u16(0),
        u16(0), u16(0), u32(crc), u32(size), u32(size),
        u16(name.length), u16(0), u16(0), u16(0), u16(0), u32(0),
        u32(localOffset), name,
      ]));
    },
  });
}

export function sessionZip(db: LocalHistoryDb, session: LocalSession,
                           events: LocalEvent[]): ReadableStream<Uint8Array> {
  async function* one(text: string) { yield text; }
  return zipStream([
    { name: 'session.json', content: one(sessionJson(session, events)) },
    { name: 'data.csv', content: csvDocument(db, session) },
    { name: 'events.csv', content: one(eventsCsv(session, events)) },
  ]);
}

// -----------------------------------------------------------------------------
//  Reading for the screen
// -----------------------------------------------------------------------------

export interface LocalSeries {
  /** Bucket centre times, ascending. */
  time: number[];
  /** Per requested channel: min, max and last within each bucket.  `null` where
   *  the bucket holds nothing — a gap stays a gap. */
  min: (number | null)[][];
  max: (number | null)[][];
  last: (number | null)[][];
  rowsRead: number;
  /** Buckets that contain no row at all: the visible holes. */
  emptyBuckets: number;
}

/**
 * Read a time range into at most `buckets` columns, streaming.
 *
 * Extremes are kept per bucket, so a 40 ms excursion in a three-day recording
 * still reaches the screen — the same promise M13 makes for the live chart, for
 * the same reason.  Peak memory is the bucket grid, never the recording.
 */
export async function readSeries(db: LocalHistoryDb, session: LocalSession,
                                 channelKeys: string[], from: number, to: number,
                                 buckets = 600): Promise<LocalSeries> {
  const indices = channelKeys.map((key) =>
    session.channels.findIndex((c) => c.key === key));
  const count = session.channels.length;
  const width = Math.max(1, (to - from) / Math.max(1, buckets));

  const time: number[] = [];
  const min: (number | null)[][] = indices.map(() => new Array(buckets).fill(null));
  const max: (number | null)[][] = indices.map(() => new Array(buckets).fill(null));
  const last: (number | null)[][] = indices.map(() => new Array(buckets).fill(null));
  const filled = new Uint8Array(buckets);
  for (let b = 0; b < buckets; ++b) time.push(from + width * (b + 0.5));

  let rowsRead = 0;
  await db.eachChunk(session.id, (chunk) => {
    for (let row = 0; row < chunk.rows; ++row) {
      const at = chunk.clientEpochMs[row]!;
      if (at < from || at > to) continue;
      const bucket = Math.min(buckets - 1, Math.floor((at - from) / width));
      rowsRead += 1;
      filled[bucket] = 1;
      for (let s = 0; s < indices.length; ++s) {
        const c = indices[s]!;
        if (c < 0) continue;
        if (!isPresent(chunk.presentMask, count, row, c)) continue;
        const value = chunk.values[row * count + c]!;
        const lo = min[s]![bucket] ?? null;
        const hi = max[s]![bucket] ?? null;
        if (lo === null || value < lo) min[s]![bucket] = value;
        if (hi === null || value > hi) max[s]![bucket] = value;
        last[s]![bucket] = value;
      }
    }
  }, from, to);

  let emptyBuckets = 0;
  for (let b = 0; b < buckets; ++b) if (!filled[b]) emptyBuckets += 1;
  return { time, min, max, last, rowsRead, emptyBuckets };
}
