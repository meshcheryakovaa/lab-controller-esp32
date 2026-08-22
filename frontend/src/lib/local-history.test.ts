// =============================================================================
//  local-history.test.ts — the local archive (npm test), M14 §21.1.
//
//  §58 again: test the failures that are SILENT.  A recorder that loses rows
//  loudly is an annoyance; one that writes the last known value across a
//  three-hour Wi-Fi outage and calls it GOOD produces a dataset that looks
//  perfect and is wrong, and nobody finds out until the paper is written.
//
//  So most of what follows is about honesty rather than function: gaps stay
//  gaps, absent is not zero, stale is not good, dropped rows are counted, and
//  nothing deletes yesterday's run to make room for today's.
// =============================================================================

import 'fake-indexeddb/auto';
import { beforeEach, describe, expect, it } from 'vitest';
import { LocalHistoryDb, QuotaExceeded } from './local-history-db';
import { ClientRecorderCore, ChunkBuilder, reconcileUnfinished,
         type RecorderChannel } from './client-recorder';
import { csvDocument, csvRows, eventsCsv, readSeries, crc32, zipStream } from './local-export';
import { acquireLease, acquireRecorderOwnership, leaseHolder,
         type LeaseStorage } from './recorder-lock';
import { MAX_QUEUED_ROWS, isPresent, qualityName, rateIntervalMs,
         type LocalChunk } from './local-history-types';
import type { ChannelFrame } from './live';

// --- helpers -----------------------------------------------------------------

let dbCounter = 0;
async function freshDb(): Promise<LocalHistoryDb> {
  // A database per test: fake-indexeddb is global, and a leaked session from a
  // previous test is exactly the cross-contamination this milestone forbids.
  return LocalHistoryDb.open(`lc-test-${++dbCounter}`);
}

function channels(n: number): RecorderChannel[] {
  return Array.from({ length: n }, (_, i) => ({
    handle: 10 + i,
    key: `dev.ch${i}`,
    name: `Channel ${i}`,
    unit: i === 0 ? 'degC' : '%',
    quantity: i === 0 ? 'temperature' : 'ratio',
    precision: 2,
  }));
}

function frame(t: number, values: Record<number, number>,
               quality: Record<number, string> = {}): ChannelFrame {
  return {
    t,
    epoch: 0,
    values: new Map(Object.entries(values).map(([h, v]) => [Number(h), v])),
    quality: new Map(Object.entries(quality).map(([h, q]) => [Number(h), q as never])),
  };
}

/** A clock the test drives, so a seven-day run takes milliseconds. */
function clock(start = 1_700_000_000_000) {
  let at = start;
  return { now: () => at, advance(ms: number) { at += ms; }, get value() { return at; } };
}

async function collect(iter: AsyncIterable<string>): Promise<string> {
  let out = '';
  for await (const piece of iter) out += piece;
  return out;
}

// --- 1, 2, 3: sessions and chunks --------------------------------------------

describe('sessions and chunks', () => {
  let db: LocalHistoryDb;
  beforeEach(async () => { db = await freshDb(); });

  it('creates a session, records, and closes it', async () => {
    const time = clock();
    const recorder = new ClientRecorderCore(db, time.now);
    const session = await recorder.start({
      controllerId: 'lc-aaa', dashboardKey: 'main', name: 'Run 1',
      firmwareVersion: '0.14.0-m14', configRevision: 7,
      rateMode: '1Hz', channels: channels(2),
    });
    expect(session.state).toBe('RECORDING');

    for (let i = 0; i < 10; ++i) {
      recorder.onFrame(frame(i * 1000, { 10: 20 + i, 11: 50 }));
      time.advance(1000);
      recorder.tick();
    }
    await recorder.stop('done');

    const stored = await db.getSession(session.id);
    expect(stored?.state).toBe('COMPLETE');
    expect(stored?.stopReason).toBe('done');
    expect(stored!.rows).toBeGreaterThan(5);
    expect(stored!.endedClientEpochMs).toBeGreaterThan(stored!.startedClientEpochMs);
  });

  it('writes several chunks and reads them back in sequence order', async () => {
    const time = clock();
    const recorder = new ClientRecorderCore(db, time.now);
    const session = await recorder.start({
      controllerId: 'lc-aaa', dashboardKey: 'main', name: 'Long',
      firmwareVersion: 'x', configRevision: 1,
      rateMode: '5Hz', channels: channels(2),
    });
    // Enough rows to force several flushes (MAX_PENDING_ROWS = 512).
    for (let i = 0; i < 2000; ++i) {
      recorder.onFrame(frame(i * 200, { 10: i, 11: -i }));
      time.advance(200);
      recorder.tick();
      if (i % 100 === 0) await recorder.flush();
    }
    await recorder.stop();

    expect(await db.chunkCount(session.id)).toBeGreaterThan(1);
    const chunks = await db.readChunks(session.id);
    for (let i = 1; i < chunks.length; ++i) {
      expect(chunks[i]!.sequence).toBeGreaterThan(chunks[i - 1]!.sequence);
      expect(chunks[i]!.startEpochMs).toBeGreaterThanOrEqual(chunks[i - 1]!.endEpochMs);
    }
    const total = chunks.reduce((n, c) => n + c.rows, 0);
    expect((await db.getSession(session.id))!.rows).toBe(total);
  });

  it('keeps one controller out of another controller\'s list', async () => {
    for (const controllerId of ['lc-aaa', 'lc-bbb', 'lc-aaa']) {
      const recorder = new ClientRecorderCore(db, clock().now);
      await recorder.start({
        controllerId, dashboardKey: 'main', name: `run on ${controllerId}`,
        firmwareVersion: 'x', configRevision: 1, rateMode: '1Hz',
        channels: channels(1),
      });
      await recorder.stop();
    }
    // Same origin, two rigs — the 192.168.4.1 case this exists for.
    expect((await db.listSessions('lc-aaa')).length).toBe(2);
    expect((await db.listSessions('lc-bbb')).length).toBe(1);
    expect((await db.listSessions()).length).toBe(3);
  });

  it('reads only the chunks a time range touches', async () => {
    const time = clock(1_000_000);
    const recorder = new ClientRecorderCore(db, time.now);
    const session = await recorder.start({
      controllerId: 'lc-aaa', dashboardKey: 'main', name: 'Range',
      firmwareVersion: 'x', configRevision: 1, rateMode: '1Hz',
      channels: channels(1),
    });
    for (let i = 0; i < 1500; ++i) {
      recorder.onFrame(frame(i * 1000, { 10: i }));
      time.advance(1000);
      recorder.tick();
      // A real page yields between frames, which is when writes actually
      // complete; a tight loop would keep every flush pending and produce one
      // enormous block, testing nothing about ranges.
      if (i % 100 === 0) await recorder.flush();
    }
    await recorder.stop();

    const all = await db.readChunks(session.id);
    expect(all.length).toBeGreaterThan(2);
    const mid = all[Math.floor(all.length / 2)]!;
    const some = await db.readChunks(session.id, mid.startEpochMs, mid.endEpochMs);
    expect(some.length).toBeLessThan(all.length);
    expect(some.some((c) => c.sequence === mid.sequence)).toBe(true);
  });
});

// --- 6, 7: absent vs zero, and quality ---------------------------------------

describe('what a row actually says', () => {
  let db: LocalHistoryDb;
  beforeEach(async () => { db = await freshDb(); });

  it('tells a real zero apart from no reading at all', async () => {
    const time = clock();
    const recorder = new ClientRecorderCore(db, time.now);
    const session = await recorder.start({
      controllerId: 'lc-aaa', dashboardKey: 'main', name: 'Zero',
      firmwareVersion: 'x', configRevision: 1, rateMode: '1Hz',
      channels: channels(2),
    });
    // Channel 0 reads exactly 0.  Channel 1 has never reported at all.
    recorder.onFrame(frame(0, { 10: 0 }));
    time.advance(1000);
    recorder.tick();
    await recorder.stop();

    const [chunk] = await db.readChunks(session.id);
    expect(chunk!.rows).toBeGreaterThan(0);
    expect(isPresent(chunk!.presentMask, 2, 0, 0)).toBe(true);
    expect(chunk!.values[0]).toBe(0);
    expect(isPresent(chunk!.presentMask, 2, 0, 1)).toBe(false);

    // And the CSV keeps the distinction: "0" versus an empty cell.
    const csv = await collect(csvRows(db, (await db.getSession(session.id))!));
    const cells = csv.trim().split('\n')[0]!.split(',');
    expect(cells[3]).toBe('0.00');
    expect(cells[5]).toBe('');
  });

  it('writes a channel that stopped updating as STALE, not GOOD', async () => {
    const time = clock();
    const recorder = new ClientRecorderCore(db, time.now);
    const session = await recorder.start({
      controllerId: 'lc-aaa', dashboardKey: 'main', name: 'Stale',
      firmwareVersion: 'x', configRevision: 1, rateMode: '1Hz',
      channels: channels(1),
    });
    recorder.onFrame(frame(0, { 10: 42 }, { 10: 'GOOD' }));
    time.advance(1000);
    recorder.tick();
    // Nothing further arrives; the value is still known, but nothing confirms it.
    time.advance(10_000);
    recorder.tick();
    await recorder.stop();

    const chunks = await db.readChunks(session.id);
    const qualities = chunks.flatMap((c) => Array.from(c.quality).map(qualityName));
    expect(qualities[0]).toBe('GOOD');
    expect(qualities[qualities.length - 1]).toBe('STALE');
  });

  it('round-trips every quality code', async () => {
    // The firmware's vocabulary, not the design sketch's: a stored code that
    // decodes to a word the instrument never uses is a mislabelled reading.
    for (const [code, name] of
         [[0, 'UNKNOWN'], [1, 'GOOD'], [2, 'STALE'], [3, 'OUT_OF_RANGE'],
          [4, 'SATURATED'], [5, 'FAULTED']] as const) {
      expect(qualityName(code)).toBe(name);
    }
    // Anything unknown decodes to UNKNOWN rather than throwing: a recording
    // written by a newer build must still open.
    expect(qualityName(99)).toBe('UNKNOWN');
  });
});

// --- 8, 9: gaps and configuration changes ------------------------------------

describe('gaps and events', () => {
  let db: LocalHistoryDb;
  beforeEach(async () => { db = await freshDb(); });

  it('records a hole when the socket drops, and writes nothing across it', async () => {
    const time = clock();
    const recorder = new ClientRecorderCore(db, time.now);
    const session = await recorder.start({
      controllerId: 'lc-aaa', dashboardKey: 'main', name: 'Outage',
      firmwareVersion: 'x', configRevision: 1, rateMode: '1Hz',
      channels: channels(1),
    });
    for (let i = 0; i < 5; ++i) {
      recorder.onFrame(frame(i * 1000, { 10: i }));
      time.advance(1000);
      recorder.tick();
    }
    const beforeOutage = (await db.getSession(session.id))!.rows
      + (await db.readChunks(session.id)).reduce((n, c) => n + c.rows, 0);

    await recorder.noteDisconnected();
    // Half an hour of nothing.  The grid must not manufacture 1800 rows.
    for (let i = 0; i < 1800; ++i) { time.advance(1000); recorder.tick(); }
    await recorder.noteReconnected();
    for (let i = 0; i < 5; ++i) {
      recorder.onFrame(frame(100_000 + i * 1000, { 10: 99 }));
      time.advance(1000);
      recorder.tick();
    }
    await recorder.stop();

    const stored = (await db.getSession(session.id))!;
    expect(stored.gaps).toBe(1);
    expect(stored.rows).toBeLessThan(beforeOutage + 30);

    const events = await db.listEvents(session.id);
    expect(events.map((e) => e.type)).toContain('WS_DISCONNECTED');
    expect(events.map((e) => e.type)).toContain('WS_RECONNECTED');

    // And the hole is visible in the data: a bucket with nothing in it.
    const series = await readSeries(db, stored, ['dev.ch0'],
                                    stored.startedClientEpochMs,
                                    stored.endedClientEpochMs!, 60);
    expect(series.emptyBuckets).toBeGreaterThan(10);
  });

  it('notes a configuration change and a device restart while recording', async () => {
    const time = clock();
    const recorder = new ClientRecorderCore(db, time.now);
    const session = await recorder.start({
      controllerId: 'lc-aaa', dashboardKey: 'main', name: 'Changes',
      firmwareVersion: 'x', configRevision: 1, rateMode: '1Hz',
      channels: channels(1),
    });
    recorder.onFrame(frame(500_000, { 10: 1 }));
    await recorder.noteConfigChanged(43);
    // The board rebooted: device time restarts near zero.
    recorder.onFrame(frame(12, { 10: 1 }));
    await recorder.mark('added solvent');
    await recorder.stop();

    const types = (await db.listEvents(session.id)).map((e) => e.type);
    expect(types).toContain('CONFIG_CHANGED');
    expect(types).toContain('DEVICE_RESTARTED');
    const mark = (await db.listEvents(session.id)).find((e) => e.type === 'MARK');
    expect(mark?.label).toBe('added solvent');
  });
});

// --- 10: recovery ------------------------------------------------------------

describe('a page that went away mid-recording', () => {
  it('marks the session INTERRUPTED and never silently appends to it', async () => {
    const name = `lc-test-recover-${++dbCounter}`;
    let db = await LocalHistoryDb.open(name);
    const time = clock();
    const recorder = new ClientRecorderCore(db, time.now);
    const session = await recorder.start({
      controllerId: 'lc-aaa', dashboardKey: 'main', name: 'Crashed',
      firmwareVersion: 'x', configRevision: 1, rateMode: '1Hz',
      channels: channels(1),
    });
    recorder.onFrame(frame(0, { 10: 5 }));
    time.advance(1000);
    recorder.tick();
    await recorder.flush();
    db.close();                       // the tab died here: no stop(), no close

    db = await LocalHistoryDb.open(name);
    const marked = await reconcileUnfinished(db, 'lc-aaa');
    expect(marked.map((s) => s.id)).toContain(session.id);
    expect((await db.getSession(session.id))!.state).toBe('INTERRUPTED');

    // Continuing is a NEW session that names the old one — the missing period
    // cannot be proven empty, so it is not papered over.
    const resumed = new ClientRecorderCore(db, time.now);
    const next = await resumed.start({
      controllerId: 'lc-aaa', dashboardKey: 'main', name: 'Crashed (part 2)',
      firmwareVersion: 'x', configRevision: 1, rateMode: '1Hz',
      channels: channels(1), parentSessionId: session.id,
    });
    expect(next.id).not.toBe(session.id);
    expect(next.parentSessionId).toBe(session.id);
    await resumed.stop();
  });
});

// --- 11, 12: full disk and a slow database -----------------------------------

describe('when the device runs out of room or falls behind', () => {
  it('stops the recording as FULL and leaves earlier sessions alone', async () => {
    const db = await freshDb();
    const time = clock();
    const recorder = new ClientRecorderCore(db, time.now);
    const session = await recorder.start({
      controllerId: 'lc-aaa', dashboardKey: 'main', name: 'Fills up',
      firmwareVersion: 'x', configRevision: 1, rateMode: '1Hz',
      channels: channels(1),
    });
    recorder.onFrame(frame(0, { 10: 1 }));
    time.advance(1000);
    recorder.tick();

    // The next write is refused for lack of space.
    const original = db.appendChunk.bind(db);
    db.appendChunk = async () => { throw new QuotaExceeded(); };
    await recorder.flush();
    db.appendChunk = original;

    const stored = (await db.getSession(session.id))!;
    expect(stored.state).toBe('FULL');
    expect((await db.listEvents(session.id)).map((e) => e.type))
      .toContain('QUOTA_EXCEEDED');
    expect(recorder.active).toBe(false);
  });

  it('drops rows and counts them rather than growing without limit', async () => {
    const db = await freshDb();
    const time = clock();
    const recorder = new ClientRecorderCore(db, time.now);
    await recorder.start({
      controllerId: 'lc-aaa', dashboardKey: 'main', name: 'Backpressure',
      firmwareVersion: 'x', configRevision: 1, rateMode: 'every',
      channels: channels(4),
    });
    // A database that never completes a write: the queue must hit its ceiling
    // and stay there instead of eating the tablet's memory.
    db.appendChunk = () => new Promise<number>(() => {});
    for (let i = 0; i < MAX_QUEUED_ROWS * 3; ++i) {
      recorder.onFrame(frame(i, { 10: i, 11: i, 12: i, 13: i }));
      time.advance(1);
    }
    const snapshot = recorder.snapshot();
    expect(snapshot.droppedRows).toBeGreaterThan(0);
    // Everything past the ceiling was dropped, not buffered.
    expect(snapshot.droppedRows).toBeGreaterThanOrEqual(MAX_QUEUED_ROWS);
  });

  it('caps a single block at the hard ceiling', () => {
    const builder = new ChunkBuilder(2, 4);
    let accepted = 0;
    for (let i = 0; i < MAX_QUEUED_ROWS + 500; ++i) {
      if (builder.append(i, i, () => 1, () => 'GOOD')) accepted += 1;
    }
    expect(accepted).toBe(MAX_QUEUED_ROWS);
    expect(builder.rows).toBe(MAX_QUEUED_ROWS);
  });
});

// --- 13: nothing disappears on its own ---------------------------------------

describe('retention', () => {
  it('never deletes an old session to make room for a new one', async () => {
    const db = await freshDb();
    const ids: string[] = [];
    for (let i = 0; i < 5; ++i) {
      const recorder = new ClientRecorderCore(db, clock(1_000 + i * 100_000).now);
      const session = await recorder.start({
        controllerId: 'lc-aaa', dashboardKey: 'main', name: `Run ${i}`,
        firmwareVersion: 'x', configRevision: 1, rateMode: '1Hz',
        channels: channels(1),
      });
      recorder.onFrame(frame(0, { 10: i }));
      await recorder.flush();
      await recorder.stop();
      ids.push(session.id);
    }
    const listed = await db.listSessions('lc-aaa');
    expect(listed.length).toBe(5);
    for (const id of ids) expect(await db.getSession(id)).toBeDefined();

    // Deletion is a user's act, and it takes the chunks and events with it.
    await db.deleteSession(ids[0]!);
    expect(await db.getSession(ids[0]!)).toBeUndefined();
    expect(await db.chunkCount(ids[0]!)).toBe(0);
    expect((await db.listSessions('lc-aaa')).length).toBe(4);
  });
});

// --- 14: export --------------------------------------------------------------

describe('export', () => {
  it('streams the CSV a chunk at a time instead of building one string', async () => {
    const db = await freshDb();
    const time = clock();
    const recorder = new ClientRecorderCore(db, time.now);
    const session = await recorder.start({
      controllerId: 'lc-aaa', dashboardKey: 'main', name: 'Export',
      firmwareVersion: '0.14.0-m14', configRevision: 3, rateMode: '1Hz',
      channels: channels(2),
    });
    for (let i = 0; i < 1500; ++i) {
      recorder.onFrame(frame(i * 1000, { 10: i / 10, 11: 50 }, { 10: 'GOOD', 11: 'GOOD' }));
      time.advance(1000);
      recorder.tick();
    }
    await recorder.stop();
    const stored = (await db.getSession(session.id))!;

    let pieces = 0;
    let lines = 0;
    let biggest = 0;
    for await (const piece of csvDocument(db, stored)) {
      pieces += 1;
      biggest = Math.max(biggest, piece.length);
      lines += piece.split('\n').length - 1;
    }
    // More than one piece is the property under test: a single yield would mean
    // the whole file was assembled in memory first.
    expect(pieces).toBeGreaterThan(2);
    expect(lines).toBeGreaterThan(stored.rows);
    expect(biggest).toBeLessThan(1_000_000);
  });

  it('puts the reproducibility header and the gap count in the CSV', async () => {
    const db = await freshDb();
    const recorder = new ClientRecorderCore(db, clock().now);
    const session = await recorder.start({
      controllerId: 'lc-7c9ebd31a240', dashboardKey: 'laboratory', name: 'Header',
      operator: 'Alexander', sample: 'TEOS-07',
      firmwareVersion: '0.14.0-m14', configRevision: 42, rateMode: '1Hz',
      channels: channels(1),
    });
    recorder.onFrame(frame(0, { 10: 1 }));
    await recorder.flush();
    await recorder.stop();
    const text = await collect(csvDocument(db, (await db.getSession(session.id))!));
    expect(text).toContain('# source: client IndexedDB');
    expect(text).toContain('# controller_id: lc-7c9ebd31a240');
    expect(text).toContain('# operator: Alexander');
    expect(text).toContain('# sample: TEOS-07');
    expect(text).toContain('# config_revision: 42');
    expect(text).toContain('client_epoch_ms,client_iso,device_ms,dev.ch0[degC],dev.ch0.quality');
  });

  it('writes a ZIP a reader can open', async () => {
    async function* one(text: string) { yield text; }
    const stream = zipStream([{ name: 'a.txt', content: one('hello') }]);
    const parts: Uint8Array[] = [];
    const reader = stream.getReader();
    for (;;) {
      const { done, value } = await reader.read();
      if (done) break;
      if (value) parts.push(value);
    }
    const total = parts.reduce((n, p) => n + p.length, 0);
    const bytes = new Uint8Array(total);
    let at = 0;
    for (const part of parts) { bytes.set(part, at); at += part.length; }
    const view = new DataView(bytes.buffer);
    expect(view.getUint32(0, true)).toBe(0x04034b50);                 // local header
    expect(view.getUint32(total - 22, true)).toBe(0x06054b50);        // end of directory
    expect(crc32(new TextEncoder().encode('hello'))).toBe(0x3610a686);
  });

  it('exports events as their own rows', async () => {
    const db = await freshDb();
    const recorder = new ClientRecorderCore(db, clock().now);
    const session = await recorder.start({
      controllerId: 'lc-aaa', dashboardKey: 'main', name: 'Events',
      firmwareVersion: 'x', configRevision: 1, rateMode: '1Hz',
      channels: channels(1),
    });
    await recorder.mark('heater on');
    await recorder.stop();
    const csv = eventsCsv((await db.getSession(session.id))!,
                          await db.listEvents(session.id));
    expect(csv).toContain('MARK');
    expect(csv).toContain('heater on');
  });
});

// --- 5, 16: reading a range for the screen ------------------------------------

describe('reading a session for the screen', () => {
  it('bounds the work by the buckets asked for, not by the rows stored', async () => {
    const db = await freshDb();
    const time = clock(0);
    const recorder = new ClientRecorderCore(db, time.now);
    const session = await recorder.start({
      controllerId: 'lc-aaa', dashboardKey: 'main', name: 'Big',
      firmwareVersion: 'x', configRevision: 1, rateMode: '5Hz',
      channels: channels(2),
    });
    // A spike of one row in twelve thousand must still reach the screen.
    for (let i = 0; i < 12_000; ++i) {
      recorder.onFrame(frame(i * 200, { 10: i === 7000 ? 999 : Math.sin(i / 50), 11: 1 }));
      time.advance(200);
      recorder.tick();
      if (i % 250 === 0) await recorder.flush();   // yield, as a browser does
    }
    await recorder.stop();
    const stored = (await db.getSession(session.id))!;

    const series = await readSeries(db, stored, ['dev.ch0'], 0, time.value, 500);
    expect(series.time.length).toBe(500);
    expect(series.rowsRead).toBeGreaterThan(10_000);
    const peak = Math.max(...series.max[0]!.filter((v): v is number => v !== null));
    expect(peak).toBeCloseTo(999, 0);
  });
});

// --- 15: two tabs ------------------------------------------------------------

describe('two tabs', () => {
  function fakeStorage(): LeaseStorage {
    const map = new Map<string, string>();
    return {
      getItem: (k) => map.get(k) ?? null,
      setItem: (k, v) => { map.set(k, v); },
      removeItem: (k) => { map.delete(k); },
    };
  }

  it('lets only one tab hold the recorder for a controller', async () => {
    const storage = fakeStorage();
    const first = acquireLease(storage, 'lc-aaa', 'tab-one', 1000);
    expect(first).not.toBeNull();
    expect(acquireLease(storage, 'lc-aaa', 'tab-two', 1500)).toBeNull();
    expect(leaseHolder(storage, 'lc-aaa', 1500)).toBe('tab-one');

    // A different controller is a different lock: two rigs, two recordings.
    expect(acquireLease(storage, 'lc-bbb', 'tab-two', 1500)).not.toBeNull();
  });

  it('lets another tab take over once a crashed one stops its heartbeat', () => {
    const storage = fakeStorage();
    acquireLease(storage, 'lc-aaa', 'tab-one', 1000);
    // Still alive at +5 s...
    expect(acquireLease(storage, 'lc-aaa', 'tab-two', 6000)).toBeNull();
    // ...and abandoned by +20 s, which is what makes a crash recoverable.
    expect(acquireLease(storage, 'lc-aaa', 'tab-two', 21_000)).not.toBeNull();
    expect(leaseHolder(storage, 'lc-aaa', 21_000)).toBe('tab-two');
  });

  it('will not let one tab release another tab\'s lease', () => {
    const storage = fakeStorage();
    const first = acquireLease(storage, 'lc-aaa', 'tab-one', 1000)!;
    const later = acquireLease(storage, 'lc-aaa', 'tab-two', 30_000)!;
    first.release();
    expect(leaseHolder(storage, 'lc-aaa', 30_100)).toBe('tab-two');
    later.release();
    expect(leaseHolder(storage, 'lc-aaa', 30_200)).toBeNull();
  });

  it('falls back to the lease when Web Locks is absent', async () => {
    const storage = fakeStorage();
    const held = await acquireRecorderOwnership('lc-aaa', 'tab-one',
      { storage, locks: null, now: () => 1000 });
    expect(held).not.toBeNull();
    const denied = await acquireRecorderOwnership('lc-aaa', 'tab-two',
      { storage, locks: null, now: () => 2000 });
    expect(denied).toBeNull();
  });
});

// --- rate modes ---------------------------------------------------------------

describe('rate modes', () => {
  it('maps every mode to an interval, and only "every" to none', () => {
    expect(rateIntervalMs('0.1Hz')).toBe(10_000);
    expect(rateIntervalMs('0.2Hz')).toBe(5_000);
    expect(rateIntervalMs('1Hz')).toBe(1_000);
    expect(rateIntervalMs('5Hz')).toBe(200);
    expect(rateIntervalMs('every')).toBeNull();
  });

  it('writes one row per frame in "every", and one per grid point otherwise', async () => {
    const db = await freshDb();
    const each = new ClientRecorderCore(db, clock().now);
    const everySession = await each.start({
      controllerId: 'lc-aaa', dashboardKey: 'main', name: 'every',
      firmwareVersion: 'x', configRevision: 1, rateMode: 'every',
      channels: channels(1),
    });
    for (let i = 0; i < 20; ++i) each.onFrame(frame(i * 50, { 10: i }));
    await each.stop();
    expect((await db.getSession(everySession.id))!.rows).toBe(20);

    const time = clock();
    const slow = new ClientRecorderCore(db, time.now);
    const slowSession = await slow.start({
      controllerId: 'lc-aaa', dashboardKey: 'main', name: 'slow',
      firmwareVersion: 'x', configRevision: 1, rateMode: '0.1Hz',
      channels: channels(1),
    });
    // 20 frames a second apart, but a row only every ten seconds.
    for (let i = 0; i < 20; ++i) {
      slow.onFrame(frame(i * 1000, { 10: i }));
      time.advance(1000);
      slow.tick();
    }
    await slow.stop();
    const rows = (await db.getSession(slowSession.id))!.rows;
    expect(rows).toBeGreaterThan(0);
    expect(rows).toBeLessThanOrEqual(4);
  });
});

// --- the chunk itself ---------------------------------------------------------

describe('ChunkBuilder', () => {
  it('hands over exactly the rows it holds', () => {
    const builder = new ChunkBuilder(3, 2);
    for (let i = 0; i < 5; ++i) builder.append(1000 + i, i, (c) => c, () => 'GOOD');
    const chunk: LocalChunk = builder.take('s1', 0);
    expect(chunk.rows).toBe(5);
    expect(chunk.clientEpochMs.length).toBe(5);
    expect(chunk.values.length).toBe(15);
    expect(chunk.startEpochMs).toBe(1000);
    expect(chunk.endEpochMs).toBe(1004);
  });
});
