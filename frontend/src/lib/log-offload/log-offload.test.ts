// =============================================================================
//  log-offload.test.ts — collecting segments from the controller (npm test).
//
//  Milestone 15 is the first feature in this project where the browser causes
//  the CONTROLLER to delete measurements.  Everything here is about the
//  conditions on that: what must be true before an acknowledgement is sent, and
//  what must NOT happen when any of it fails.
//
//  The tests are written so that a wrong answer fails loudly.  The failure this
//  guards against is silent by nature — a segment acknowledged and deleted that
//  was never correctly stored leaves no trace on either side.
// =============================================================================

import 'fake-indexeddb/auto';
import { beforeEach, describe, expect, it } from 'vitest';
import { SegmentArchive, StorageFull, segmentKey } from './SegmentArchive';
import { LocalHistoryDb } from '../local-history-db';
import {
  SegmentCollector, MAX_VERIFY_FAILURES, RETRY_DELAYS_MS,
  type CollectorTransport, type PendingSegment, type SegmentQueue,
} from './SegmentCollector';
import { crc32, crc32Hex, segmentPayload, verifySegment } from './crc32';
import { findGaps, manifestJson, mergedCsv } from './SegmentExport';

// --- a controller, simulated -------------------------------------------------

const encoder = new TextEncoder();

/** A segment exactly as LogStore writes one, so the payload slicing under test
 *  is the same slicing the real files need. */
function makeSegment(sequence: number, firstRow: number, rows: number): {
  file: Uint8Array; pending: PendingSegment;
} {
  const header =
    `# dataset: log_0042\n# segment: ${sequence}\n# mode: continuous_offload\n`
    + '# name: evaporation\n# operator: Александр\n# sample: TEOS-07\n'
    + '# firmware: 0.15.0-m15\n# config_fingerprint: 8f214ca0\n'
    + `# first_global_row: ${firstRow}\n# previous_segment: ${sequence - 1}\n`
    + '# calibrations: none\n'
    + 't_ms,epoch_ms,global_row,bath.degC,quality_mask\n';
  let body = '';
  for (let i = 0; i < rows; ++i) {
    body += `${(firstRow + i) * 1000},1787351160000,${firstRow + i},42.125,0\n`;
  }
  const payloadCrc = crc32Hex(encoder.encode(body));
  const footer =
    `# segment_complete\n# segment_rows: ${rows}\n`
    + `# first_global_row: ${firstRow}\n# last_global_row: ${firstRow + rows - 1}\n`
    + '# dropped_rows_total: 0\n'
    + `# payload_bytes: ${encoder.encode(body).length}\n`
    + `# payload_crc32: ${payloadCrc}\n`;
  const file = encoder.encode(header + body + footer);
  return {
    file,
    pending: {
      sequence, bytes: file.length, rows,
      first_row: firstRow, last_row: firstRow + rows - 1,
      payload_crc32: payloadCrc, state: 'READY',
    },
  };
}

/** A controller that behaves, until a test tells it not to. */
class FakeController implements CollectorTransport {
  segments = new Map<number, Uint8Array>();
  pending: PendingSegment[] = [];
  acked: number[] = [];
  state = 'RECORDING';
  failDownload = 0;         // how many downloads to fail before succeeding
  failAck = 0;
  corruptDownload = false;
  truncateDownload = false;
  queueCalls = 0;
  downloadCalls = 0;
  ackedProof: Array<{ sequence: number; bytes: number; crc: string }> = [];

  add(sequence: number, firstRow: number, rows: number): PendingSegment {
    const { file, pending } = makeSegment(sequence, firstRow, rows);
    this.segments.set(sequence, file);
    this.pending.push(pending);
    return pending;
  }

  async queue(): Promise<SegmentQueue> {
    this.queueCalls += 1;
    return {
      session_id: 'log_0042', state: this.state, mode: 'continuous_offload',
      collector_id: 'browser-01',
      segments_completed: this.pending.length + this.acked.length,
      segments_acked: this.acked.length,
      acked_through: this.acked.length > 0 ? Math.max(...this.acked) : 0,
      rows: 1000, dropped: 0,
      active_segment: 99, active_bytes: 1024, segment_bytes: 102400,
      pending: [...this.pending],
      pending_bytes: this.pending.reduce((n, p) => n + p.bytes, 0),
      writable_bytes: 500_000,
    };
  }

  async download(_sessionId: string, sequence: number): Promise<Uint8Array> {
    this.downloadCalls += 1;
    if (this.failDownload > 0) {
      this.failDownload -= 1;
      throw new Error('connection reset');
    }
    const file = this.segments.get(sequence);
    if (!file) throw new Error('no such segment');
    if (this.truncateDownload) return file.slice(0, file.length - 20);
    if (this.corruptDownload) {
      const copy = file.slice();
      // One byte, in the rows: passes a size check and fails a checksum, which
      // is exactly the case the checksum exists for.
      const at = Math.floor(copy.length / 2);
      copy[at] = copy[at]! ^ 0x01;
      return copy;
    }
    return file;
  }

  async acknowledge(_sessionId: string, sequence: number, proof: {
    collector_id: string; bytes: number; payload_crc32: string;
  }): Promise<{ acknowledged: boolean; already_acknowledged?: boolean }> {
    if (this.failAck > 0) {
      this.failAck -= 1;
      throw new Error('the response never arrived');
    }
    const already = this.acked.includes(sequence);
    this.ackedProof.push({ sequence, bytes: proof.bytes,
                           crc: proof.payload_crc32 });
    if (!already) {
      this.acked.push(sequence);
      // The real controller deletes the file here.  So does this one.
      this.segments.delete(sequence);
      this.pending = this.pending.filter((p) => p.sequence !== sequence);
    }
    return { acknowledged: true, already_acknowledged: already };
  }
}

let dbCounter = 0;
async function freshArchive(): Promise<SegmentArchive> {
  return SegmentArchive.open(`lc-offload-test-${++dbCounter}`);
}

async function drain(collector: SegmentCollector, passes = 20): Promise<void> {
  for (let i = 0; i < passes; ++i) {
    if (!(await collector.pump())) break;
  }
}

// --- CRC32 -------------------------------------------------------------------

describe('crc32', () => {
  it('matches the standard vectors the firmware is checked against', () => {
    // The same three the C++ side uses.  Two implementations that agree only by
    // assertion are how a corrupted transfer ends with a deleted original.
    expect(crc32Hex(encoder.encode(''))).toBe('00000000');
    expect(crc32Hex(encoder.encode('a'))).toBe('e8b7be43');
    expect(crc32Hex(encoder.encode('123456789'))).toBe('cbf43926');
    expect(crc32Hex(encoder.encode('The quick brown fox jumps over the lazy dog')))
      .toBe('414fa339');
  });

  it('continues a running total exactly as one pass would', () => {
    const whole = encoder.encode('123456789');
    const first = crc32(whole.subarray(0, 4));
    expect(crc32(whole.subarray(4), first)).toBe(crc32(whole));
  });

  it('checksums the rows only — not the header, not the footer', () => {
    const { file, pending } = makeSegment(1, 1, 5);
    const payload = segmentPayload(file)!;
    const text = new TextDecoder().decode(payload);
    expect(text.startsWith('1000,')).toBe(true);
    expect(text.includes('# segment')).toBe(false);
    expect(text.includes('quality_mask')).toBe(false);
    expect(crc32Hex(payload)).toBe(pending.payload_crc32);
  });

  it('refuses a file with no footer rather than checksumming what arrived', () => {
    const { file } = makeSegment(1, 1, 5);
    // Cut INTO the footer marker, which is what a transfer that stopped part
    // way through the last write actually looks like.
    const at = new TextDecoder().decode(file).indexOf('# segment_complete');
    const cut = file.slice(0, at + 6);
    expect(segmentPayload(cut)).toBeNull();
    const result = verifySegment(cut, cut.length, '00000000');
    expect(result.ok).toBe(false);
    expect(result.reason).toContain('footer');
  });

  it('catches a wrong size before it looks at the bytes', () => {
    const { file, pending } = makeSegment(1, 1, 5);
    const result = verifySegment(file, pending.bytes + 1, pending.payload_crc32);
    expect(result.ok).toBe(false);
    expect(result.reason).toContain('bytes');
  });
});

// --- one database, two milestones ---------------------------------------------

describe('sharing the database with Milestone 14', () => {
  it('lets both modules open it at once', async () => {
    // IndexedDB allows exactly ONE version per database.  When these two used
    // different numbers the symptom was not an error: the lower-version
    // connection blocked the upgrade and the second open never settled, so the
    // page simply stopped with nothing in the console.  A hang is the worst
    // possible way to learn about a version mismatch, hence this test.
    const name = `lc-shared-${++dbCounter}`;
    const recordings = await LocalHistoryDb.open(name);
    const segments = await SegmentArchive.open(name);

    // Both sets of stores exist and work, in either order of opening.
    await recordings.putSettings({ controllerId: 'lc-aaa', dashboardKey: 'main' });
    await segments.putSession({
      key: 'lc-aaa/log_1', controllerId: 'lc-aaa', sessionId: 'log_1',
      name: 'x', operator: '', sample: '', firmwareVersion: '', rateHz: 1,
      channels: 1, startedEpochMs: 1, state: 'RECORDING', rows: 0, dropped: 0,
      segmentsCollected: 0, bytesCollected: 0, contiguousThrough: 0,
      updatedEpochMs: 1,
    });
    expect(await segments.getSession('lc-aaa', 'log_1')).toBeDefined();
    expect(await recordings.getSettings('lc-aaa', 'main')).toBeDefined();
    recordings.close();
    segments.close();
  });

  it('works when the offload archive opens the database first', async () => {
    const name = `lc-shared-${++dbCounter}`;
    const segments = await SegmentArchive.open(name);
    const recordings = await LocalHistoryDb.open(name);
    // The M14 stores must exist even though M15 performed the upgrade.
    await recordings.putSettings({ controllerId: 'lc-bbb', dashboardKey: 'main' });
    expect(await recordings.getSettings('lc-bbb', 'main')).toBeDefined();
    expect(await segments.listSessions('lc-bbb')).toEqual([]);
    recordings.close();
    segments.close();
  });
});

// --- collecting ---------------------------------------------------------------

describe('the collector', () => {
  let archive: SegmentArchive;
  let controller: FakeController;
  let collector: SegmentCollector;

  beforeEach(async () => {
    archive = await freshArchive();
    controller = new FakeController();
    collector = new SegmentCollector(archive, controller, () => 1_700_000_000_000);
  });

  it('moves segments oldest first and acknowledges each one', async () => {
    const declared = [
      controller.add(1, 1, 20),
      controller.add(2, 21, 20),
      controller.add(3, 41, 20),
    ];
    await collector.attach('lc-aaa', 'log_0042', 'browser-01');
    await drain(collector);

    expect(controller.acked).toEqual([1, 2, 3]);
    const stored = await archive.listSegments('lc-aaa', 'log_0042');
    expect(stored.map((s) => s.sequence)).toEqual([1, 2, 3]);
    // Every acknowledgement carried the exact numbers the controller checks —
    // compared against what it declared, not against a regenerated guess.
    expect(controller.ackedProof.length).toBe(3);
    for (const proof of controller.ackedProof) {
      const declaredSegment = declared.find((p) => p.sequence === proof.sequence)!;
      expect(proof.bytes).toBe(declaredSegment.bytes);
      expect(proof.crc).toBe(declaredSegment.payload_crc32);
    }
  });

  it('stores the segment BEFORE acknowledging it', async () => {
    controller.add(1, 1, 20);
    // If the order were reversed, the file would be gone from the controller at
    // the moment this check runs and the local copy would not exist yet.
    const originalAck = controller.acknowledge.bind(controller);
    let storedWhenAcked: number | undefined;
    controller.acknowledge = async (id, sequence, proof) => {
      storedWhenAcked =
        (await archive.getSegment('lc-aaa', 'log_0042', sequence))?.size;
      return originalAck(id, sequence, proof);
    };
    await collector.attach('lc-aaa', 'log_0042', 'browser-01');
    await drain(collector);
    expect(storedWhenAcked).toBeGreaterThan(0);
  });

  it('never acknowledges a download that was interrupted', async () => {
    controller.add(1, 1, 20);
    controller.failDownload = 1;
    await collector.attach('lc-aaa', 'log_0042', 'browser-01');
    await collector.pump();

    expect(controller.acked).toEqual([]);
    expect(await archive.getSegment('lc-aaa', 'log_0042', 1)).toBeUndefined();
    expect(collector.status.state).toBe('WAITING_FOR_DEVICE');
    // The controller still has it, so the next pass simply asks again.
    await drain(collector);
    expect(controller.acked).toEqual([1]);
  });

  it('never acknowledges a segment whose checksum does not match', async () => {
    controller.add(1, 1, 20);
    controller.corruptDownload = true;
    await collector.attach('lc-aaa', 'log_0042', 'browser-01');
    await collector.pump();

    expect(controller.acked).toEqual([]);
    expect(await archive.getSegment('lc-aaa', 'log_0042', 1)).toBeUndefined();
    // And the controller's copy is untouched — that is the whole point.
    expect(controller.segments.has(1)).toBe(true);
  });

  it('gives up loudly after three mismatches rather than looping for ever', async () => {
    controller.add(1, 1, 20);
    controller.corruptDownload = true;
    await collector.attach('lc-aaa', 'log_0042', 'browser-01');
    for (let i = 0; i < 10; ++i) await collector.pump();

    expect(collector.status.state).toBe('ERROR');
    expect(collector.status.lastError).toContain('verification');
    expect(controller.downloadCalls).toBe(MAX_VERIFY_FAILURES);
    expect(controller.segments.has(1)).toBe(true);
    expect(controller.acked).toEqual([]);
  });

  it('reuses a good local copy instead of downloading it again', async () => {
    const pending = controller.add(1, 1, 20);
    await collector.attach('lc-aaa', 'log_0042', 'browser-01');
    // A copy that is already here and correct — the tab was reloaded after the
    // save but before the acknowledgement.
    await archive.putSegment({
      key: segmentKey('lc-aaa', 'log_0042', 1),
      controllerId: 'lc-aaa', sessionId: 'log_0042', sequence: 1,
      filename: 'log_0042_p000001.csv',
      blob: new Blob([controller.segments.get(1)!.slice().buffer]),
      size: pending.bytes, rows: 20, firstRow: 1, lastRow: 20,
      payloadCrc32: pending.payload_crc32, receivedEpochMs: 1,
    });
    await drain(collector);

    expect(controller.downloadCalls).toBe(0);
    expect(controller.acked).toEqual([1]);
  });

  it('replaces a local copy that does not verify', async () => {
    const pending = controller.add(1, 1, 20);
    await archive.putSegment({
      key: segmentKey('lc-aaa', 'log_0042', 1),
      controllerId: 'lc-aaa', sessionId: 'log_0042', sequence: 1,
      filename: 'log_0042_p000001.csv',
      blob: new Blob([encoder.encode('this is not the segment').slice().buffer]),
      size: pending.bytes, rows: 20, firstRow: 1, lastRow: 20,
      payloadCrc32: pending.payload_crc32, receivedEpochMs: 1,
    });
    await collector.attach('lc-aaa', 'log_0042', 'browser-01');
    await drain(collector);

    expect(controller.downloadCalls).toBe(1);
    expect(controller.acked).toEqual([1]);
    const stored = await archive.getSegment('lc-aaa', 'log_0042', 1);
    expect(stored!.size).toBe(pending.bytes);
  });

  it('survives a lost acknowledgement without losing the segment', async () => {
    controller.add(1, 1, 20);
    controller.failAck = 1;
    await collector.attach('lc-aaa', 'log_0042', 'browser-01');
    await collector.pump();

    // The file is here; only the confirmation went missing.
    expect(await archive.getSegment('lc-aaa', 'log_0042', 1)).toBeDefined();
    expect(controller.acked).toEqual([]);
    await drain(collector);
    expect(controller.acked).toEqual([1]);
    // And no second download was needed, because the good copy was reused.
    expect(controller.downloadCalls).toBe(1);
  });

  it('stops without acknowledging when this device is full', async () => {
    controller.add(1, 1, 20);
    archive.putSegment = async () => { throw new StorageFull(); };
    await collector.attach('lc-aaa', 'log_0042', 'browser-01');
    await collector.pump();

    expect(collector.status.state).toBe('LOCAL_STORAGE_FULL');
    expect(controller.acked).toEqual([]);
    expect(controller.segments.has(1)).toBe(true);
  });

  it('waits, rather than failing, when the controller is unreachable', async () => {
    controller.add(1, 1, 20);
    controller.queue = async () => { throw new Error('network down'); };
    await collector.attach('lc-aaa', 'log_0042', 'browser-01');
    await collector.pump();
    expect(collector.status.state).toBe('WAITING_FOR_DEVICE');
    expect(collector.status.lastError).toContain('network');
  });

  it('reports COMPLETE only when the run is over and the queue is empty', async () => {
    controller.add(1, 1, 20);
    await collector.attach('lc-aaa', 'log_0042', 'browser-01');
    await drain(collector);
    // Queue empty, but the controller is still recording: not finished.
    expect(collector.status.state).toBe('WAITING_FOR_DEVICE');

    controller.state = 'COMPLETE_OFFLOADED';
    await collector.pump();
    expect(collector.status.state).toBe('COMPLETE');
  });

  it('backs off, and stops lengthening the wait at the cap', () => {
    const delays = [];
    for (let attempts = 0; attempts < 8; ++attempts) {
      collector.status = { ...collector.status, attempts };
      delays.push(collector.retryDelayMs());
    }
    expect(delays.slice(0, 5)).toEqual(RETRY_DELAYS_MS);
    expect(delays[7]).toBe(RETRY_DELAYS_MS[RETRY_DELAYS_MS.length - 1]);
  });
});

// --- export -------------------------------------------------------------------

describe('export', () => {
  let archive: SegmentArchive;
  let controller: FakeController;

  beforeEach(async () => {
    archive = await freshArchive();
    controller = new FakeController();
    controller.add(1, 1, 10);
    controller.add(2, 11, 10);
    controller.add(3, 21, 10);
    const collector = new SegmentCollector(archive, controller, () => 1);
    await collector.attach('lc-aaa', 'log_0042', 'browser-01');
    await drain(collector);
  });

  it('merges into one CSV with exactly one column line', async () => {
    const session = (await archive.getSession('lc-aaa', 'log_0042'))!;
    let text = '';
    let pieces = 0;
    for await (const piece of mergedCsv(archive, session)) { text += piece; pieces += 1; }

    expect(pieces).toBeGreaterThan(3);            // streamed, not assembled
    expect(text.split('quality_mask\n').length - 1).toBe(1);
    // The per-segment footers are metadata about one part and must not appear
    // among the rows of the merged file.
    expect(text.split('# segment_complete').length - 1).toBe(0);
    // Rows in global order, with none missing.
    const rows = text.split('\n').filter((line) => /^\d+,/.test(line));
    expect(rows.length).toBe(30);
    expect(rows[0]!.split(',')[2]).toBe('1');
    expect(rows[29]!.split(',')[2]).toBe('30');
    expect(text).toContain('# complete: every segment');
  });

  it('marks a merge INCOMPLETE when a part is not here', async () => {
    await archive.deleteSegment('lc-aaa', 'log_0042', 2);
    const session = (await archive.getSession('lc-aaa', 'log_0042'))!;
    let text = '';
    for await (const piece of mergedCsv(archive, session)) text += piece;

    // A merged export that quietly skipped a part would be indistinguishable
    // from a complete one.
    expect(text).toContain('# INCOMPLETE: segments 2');
    expect(text).not.toContain('# complete: every segment');
  });

  it('finds the gaps between the parts it holds', () => {
    const at = (sequence: number) => ({ sequence } as never);
    expect(findGaps([at(1), at(2), at(3)])).toEqual([]);
    expect(findGaps([at(1), at(4)])).toEqual([2, 3]);
    expect(findGaps([at(2), at(5), at(6)])).toEqual([3, 4]);
  });

  it('writes a manifest that names every part and its checksum', async () => {
    const session = (await archive.getSession('lc-aaa', 'log_0042'))!;
    const segments = await archive.listSegments('lc-aaa', 'log_0042');
    const manifest = JSON.parse(manifestJson(session, segments));
    expect(manifest.segments.length).toBe(3);
    expect(manifest.segments[0].payload_crc32).toMatch(/^[0-9a-f]{8}$/);
    expect(manifest.missing_segments).toEqual([]);
    expect(manifest.session.controller_id).toBe('lc-aaa');
  });
});
