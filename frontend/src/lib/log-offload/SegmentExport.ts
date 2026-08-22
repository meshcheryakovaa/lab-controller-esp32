// =============================================================================
//  log-offload/SegmentExport.ts — putting the parts back together (M15 §20).
//
//  Two exports, and the ZIP is the safer of the two on purpose: it copies each
//  segment byte for byte, so whatever the controller wrote is what the operator
//  keeps, checksums and footers included.  The merged CSV is the convenient one
//  and is therefore the one that can lie — so it says so, out loud, in its own
//  footer whenever a part is missing.
//
//  Both stream.  A week of segments is hundreds of megabytes, and neither the
//  merged file nor the archive is ever held whole: parts are read from
//  IndexedDB one at a time and pushed straight into the stream.
// =============================================================================

import { zipStream } from '../local-export';
import type { SegmentArchive, StoredEspSegment, StoredEspSession } from './SegmentArchive';

const COLUMN_MARKER = 'quality_mask\n';

export interface MergeReport {
  segments: number;
  rows: number;
  missing: number[];
  complete: boolean;
}

/** What is missing between the parts that are here (§20.2). */
export function findGaps(segments: StoredEspSegment[]): number[] {
  const missing: number[] = [];
  const ordered = [...segments].sort((a, b) => a.sequence - b.sequence);
  for (let i = 1; i < ordered.length; ++i) {
    for (let n = ordered[i - 1]!.sequence + 1; n < ordered[i]!.sequence; ++n) {
      missing.push(n);
    }
  }
  return missing;
}

function bodyOf(text: string): string {
  // Everything after the column line, minus the segment's own footer.  The
  // footer lines are metadata about ONE part and must not end up inside the
  // rows of a merged file, where a reader would take them for data.
  const columns = text.indexOf(COLUMN_MARKER);
  const start = columns < 0 ? 0 : columns + COLUMN_MARKER.length;
  const footer = text.indexOf('# segment_complete', start);
  return footer < 0 ? text.slice(start) : text.slice(start, footer);
}

/**
 * One CSV from many segments.
 *
 * The header comes from the first part and the rest are dropped, so the file
 * has exactly one column line.  Rows are copied unchanged and in `sequence`
 * order — never re-sorted by value, because `global_row` is what proves the
 * order and re-sorting would hide a part that arrived out of place.
 */
export async function* mergedCsv(archive: SegmentArchive, session: StoredEspSession,
                                 report?: MergeReport): AsyncGenerator<string> {
  const segments = await archive.listSegments(session.controllerId, session.sessionId);
  const missing = findGaps(segments);
  let rows = 0;

  if (segments.length === 0) {
    yield '# no segments of this session are stored on this device\n';
    if (report) { report.segments = 0; report.rows = 0; report.missing = []; report.complete = false; }
    return;
  }

  const first = new TextDecoder().decode(
    new Uint8Array(await segments[0]!.blob.arrayBuffer()));
  const columns = first.indexOf(COLUMN_MARKER);
  yield columns < 0 ? '' : first.slice(0, columns + COLUMN_MARKER.length);

  for (const segment of segments) {
    const text = new TextDecoder().decode(
      new Uint8Array(await segment.blob.arrayBuffer()));
    yield bodyOf(text);
    rows += segment.rows;
  }

  // The footer states what this file is made of.  A merged export that quietly
  // skipped a part would be indistinguishable from a complete one, which is the
  // failure §20.2 is written against.
  const lines = [
    '# merged_from: client archive',
    `# session: ${session.sessionId}`,
    `# controller_id: ${session.controllerId}`,
    `# segments: ${segments.map((s) => s.sequence).join(' ')}`,
    `# checksums: ${segments.map((s) => `${s.sequence}=${s.payloadCrc32}`).join(' ')}`,
    `# rows: ${rows}`,
  ];
  if (missing.length > 0) {
    lines.push(`# INCOMPLETE: segments ${missing.join(', ')} are not on this device`,
               '# the rows around those numbers are NOT contiguous');
  } else {
    lines.push('# complete: every segment from the first to the last is present');
  }
  yield `${lines.join('\n')}\n`;

  if (report) {
    report.segments = segments.length;
    report.rows = rows;
    report.missing = missing;
    report.complete = missing.length === 0;
  }
}

export function manifestJson(session: StoredEspSession,
                             segments: StoredEspSegment[]): string {
  return `${JSON.stringify({
    session: {
      controller_id: session.controllerId,
      session_id: session.sessionId,
      name: session.name,
      operator: session.operator,
      sample: session.sample,
      firmware: session.firmwareVersion,
      rate_hz: session.rateHz,
      channels: session.channels,
      started_epoch_ms: session.startedEpochMs,
      state: session.state,
      rows_reported_by_controller: session.rows,
      dropped_reported_by_controller: session.dropped,
    },
    segments: segments.map((s) => ({
      sequence: s.sequence,
      filename: s.filename,
      bytes: s.size,
      rows: s.rows,
      first_row: s.firstRow,
      last_row: s.lastRow,
      payload_crc32: s.payloadCrc32,
      received_epoch_ms: s.receivedEpochMs,
      acknowledged_epoch_ms: s.acknowledgedEpochMs ?? null,
    })),
    missing_segments: findGaps(segments),
  }, null, 2)}\n`;
}

/**
 * The archive export: every part exactly as the controller wrote it, plus a
 * manifest.  This is the one to keep — a merged CSV is a convenience derived
 * from these files, and can be rebuilt from them at any time.
 */
export async function sessionZip(archive: SegmentArchive,
                                 session: StoredEspSession): Promise<ReadableStream<Uint8Array>> {
  const segments = await archive.listSegments(session.controllerId, session.sessionId);
  async function* one(text: string) { yield text; }
  async function* file(segment: StoredEspSegment) {
    // Decoded and re-encoded by the ZIP writer, which is lossless for the UTF-8
    // these files are: the alternative is a binary entry API the writer does
    // not need for anything else.
    yield new TextDecoder().decode(new Uint8Array(await segment.blob.arrayBuffer()));
  }
  return zipStream([
    { name: `${session.sessionId}/manifest.json`,
      content: one(manifestJson(session, segments)) },
    ...segments.map((segment) => ({
      name: `${session.sessionId}/${segment.filename}`,
      content: file(segment),
    })),
  ]);
}
