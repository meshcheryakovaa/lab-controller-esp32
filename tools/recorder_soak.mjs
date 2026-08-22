// =============================================================================
//  tools/recorder_soak.mjs — the long local recording (M14 §21.2).
//
//  The failure being reproduced is not a crash on the bench: it is a tablet
//  left recording overnight that is dead by morning, or an export that takes
//  the tab down with it.  Both come from the same mistake — holding the
//  recording in memory — so the numbers that matter are heap over time, rows
//  per stored block, and whether an export ever materialises the whole set.
//
//  Runs the real lib/client-recorder.ts and lib/local-export.ts.
//
//  Two phases, because they answer different questions:
//    A. A COUNTING database, so seven days of rows can actually be pushed
//       through the recorder.  Answers: does the recorder's own memory grow
//       with duration, and does it write blocks rather than rows?
//    B. A real (in-memory) IndexedDB at a size that fits.  Answers: does
//       storage grow linearly with rows, and does export stream?
//
//  Usage:  node --expose-gc tools/recorder_soak.mjs [--quick]
// =============================================================================
import { pathToFileURL } from 'node:url';
import { mkdtempSync, rmSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join } from 'node:path';

const here = new URL('.', import.meta.url).pathname;
const modules = `${here}../frontend/node_modules`;

// fake-indexeddb and esbuild both live in the frontend's dependencies; this
// tool is not a package of its own and deliberately does not add a second copy.
await import(pathToFileURL(`${modules}/fake-indexeddb/auto/index.mjs`).href);
const esbuild = await import(pathToFileURL(`${modules}/esbuild/lib/main.js`).href);

// Bundled, not imported file by file: the sources use extensionless specifiers
// the way a bundler resolves them, and a soak test that ran against a hand-made
// copy of the recorder would prove nothing about the one in the browser.
const out = mkdtempSync(join(tmpdir(), 'recorder-soak-'));
await esbuild.build({
  entryPoints: [`${here}../frontend/src/lib/soak-entry.ts`],
  outfile: join(out, 'bundle.mjs'),
  format: 'esm',
  bundle: true,
  logLevel: 'error',
});
const {
  ClientRecorderCore, LocalHistoryDb, csvDocument, MAX_QUEUED_ROWS,
} = await import(pathToFileURL(join(out, 'bundle.mjs')).href);

const quick = process.argv.includes('--quick');

function heap() {
  if (global.gc) global.gc();
  const usage = process.memoryUsage();
  return usage.heapUsed + usage.arrayBuffers;
}

function channels(n) {
  return Array.from({ length: n }, (_, i) => ({
    handle: 100 + i,
    key: `soak.ch${i}`,
    name: `Channel ${i}`,
    unit: i % 2 ? '%' : 'degC',
    quantity: 'test',
    precision: 3,
  }));
}

function frame(t, handles, value) {
  const values = new Map();
  const quality = new Map();
  for (const h of handles) { values.set(h, value); quality.set(h, 'GOOD'); }
  return { t, epoch: 0, values, quality };
}

/**
 * A database that accepts blocks and remembers only their shape.  Phase A is
 * about the recorder, and a real store would run out of memory long before the
 * recorder did — which would measure the wrong component.
 */
function countingDb() {
  const stats = { chunks: 0, rows: 0, bytes: 0, sessions: new Map(), events: 0 };
  return {
    stats,
    async putSession(session) { stats.sessions.set(session.id, session); },
    async patchSession(id, patch) {
      const merged = { ...stats.sessions.get(id), ...patch };
      stats.sessions.set(id, merged);
      return merged;
    },
    async getSession(id) { return stats.sessions.get(id); },
    async appendChunk(chunk) {
      stats.chunks += 1;
      stats.rows += chunk.rows;
      const bytes = chunk.clientEpochMs.byteLength + chunk.deviceMs.byteLength
                  + chunk.values.byteLength + chunk.quality.byteLength
                  + chunk.presentMask.byteLength;
      stats.bytes += bytes;
      return bytes;
    },
    async appendEvent() { stats.events += 1; },
    async listEvents() { return []; },
    async getSettings() { return undefined; },
    async putSettings() {},
    async findUnfinished() { return []; },
  };
}

async function phaseA({ name, channelCount, hz, hours }) {
  const ran = quick ? Math.min(hours, 1) : hours;
  const rows = Math.round(ran * 3600 * hz);
  const step = 1000 / hz;
  const db = countingDb();
  let at = 1_700_000_000_000;
  const recorder = new ClientRecorderCore(db, () => at);
  const chans = channels(channelCount);
  const handles = chans.map((c) => c.handle);

  await recorder.start({
    controllerId: 'lc-soak', dashboardKey: 'soak', name,
    firmwareVersion: '0.14.0-m14', configRevision: 1,
    rateMode: hz >= 50 ? 'every' : '5Hz', channels: chans,
  });

  const baseline = heap();
  const marks = [];
  const started = process.hrtime.bigint();

  for (let i = 0; i < rows; ++i) {
    at += step;
    recorder.onFrame(frame(i * step, handles, Math.sin(i / 500) * 10));
    recorder.tick(at);
    // Yield now and then, exactly as a browser does between frames; without
    // this the writes never complete and the ceiling is all we would measure.
    if ((i & 0x3FF) === 0) await recorder.flush();
    if (i > 0 && i % Math.round(3600 * hz) === 0) {
      marks.push({ hour: Math.round(i / hz / 3600), heapMiB: (heap() - baseline) / 1048576 });
    }
  }
  await recorder.stop();
  const ms = Number(process.hrtime.bigint() - started) / 1e6;

  const stats = db.stats;
  const perChunk = stats.chunks > 0 ? stats.rows / stats.chunks : 0;
  console.log(`\n=== A: ${channelCount} channels x ${hz} Hz x ${ran} h ===`);
  console.log(`  ${rows.toLocaleString()} rows in ${(ms / 1000).toFixed(1)} s`);
  for (const mark of marks.filter((_, i, a) => i === 0 || i === a.length - 1
                                            || i === Math.floor(a.length / 2))) {
    console.log(`  after ${String(mark.hour).padStart(3)} h: heap +${mark.heapMiB.toFixed(1)} MiB`);
  }
  const grew = (heap() - baseline) / 1048576;
  console.log(`  stored blocks: ${stats.chunks.toLocaleString()},`
            + ` ${perChunk.toFixed(0)} rows per block`);
  console.log(`  rows written: ${stats.rows.toLocaleString()}`
            + `, dropped: ${recorder.snapshot().droppedRows}`);
  console.log(`  heap at the end: +${grew.toFixed(1)} MiB`);

  const bounded = grew < 32;
  const blocky = perChunk > 50;                 // not one object per sample
  const queueHeld = recorder.snapshot().pendingRows <= MAX_QUEUED_ROWS;
  console.log(`  memory bounded: ${bounded ? 'yes' : 'NO'}`);
  console.log(`  written as blocks, not per sample: ${blocky ? 'yes' : 'NO'}`);
  console.log(`  queue stayed under its ceiling: ${queueHeld ? 'yes' : 'NO'}`);
  return bounded && blocky && queueHeld;
}

async function phaseB() {
  const minutes = quick ? 10 : 60;
  const hz = 5;
  const channelCount = 8;
  const rows = minutes * 60 * hz;
  const db = await LocalHistoryDb.open(`soak-${Date.now()}`);
  let at = 1_700_000_000_000;
  const recorder = new ClientRecorderCore(db, () => at);
  const chans = channels(channelCount);
  const handles = chans.map((c) => c.handle);

  const session = await recorder.start({
    controllerId: 'lc-soak', dashboardKey: 'soak', name: 'stored',
    firmwareVersion: '0.14.0-m14', configRevision: 1,
    rateMode: '5Hz', channels: chans,
  });

  const halfway = [];
  for (let i = 0; i < rows; ++i) {
    at += 200;
    recorder.onFrame(frame(i * 200, handles, Math.sin(i / 300) * 5));
    recorder.tick(at);
    if ((i & 0xFF) === 0) await recorder.flush();
    if (i === Math.floor(rows / 2)) {
      const mid = await db.getSession(session.id);
      halfway.push({ rows: mid.rows, bytes: mid.bytes });
    }
  }
  await recorder.stop();
  const stored = await db.getSession(session.id);
  const chunks = await db.chunkCount(session.id);

  // Linear: twice the rows, about twice the bytes.  A super-linear index would
  // show up here long before it showed up on a tablet at 3 a.m.
  const ratio = halfway[0].bytes > 0 ? stored.bytes / halfway[0].bytes : 0;

  let pieces = 0;
  let biggest = 0;
  let total = 0;
  for await (const piece of csvDocument(db, stored)) {
    pieces += 1;
    biggest = Math.max(biggest, piece.length);
    total += piece.length;
  }

  console.log(`\n=== B: ${channelCount} channels x ${hz} Hz x ${minutes} min, real IndexedDB ===`);
  console.log(`  ${stored.rows.toLocaleString()} rows in ${chunks} blocks,`
            + ` ${(stored.bytes / 1024).toFixed(0)} KiB`);
  console.log(`  growth from halfway: ${ratio.toFixed(2)}x rows,`
            + ` ${(stored.rows / halfway[0].rows).toFixed(2)}x bytes`);
  console.log(`  CSV: ${(total / 1024).toFixed(0)} KiB in ${pieces} pieces,`
            + ` largest piece ${(biggest / 1024).toFixed(0)} KiB`);

  const linear = ratio > 1.6 && ratio < 2.5;
  const streamed = pieces > 3 && biggest < 512 * 1024;
  console.log(`  storage grows linearly: ${linear ? 'yes' : 'NO'}`);
  console.log(`  export streams rather than materialising: ${streamed ? 'yes' : 'NO'}`);
  db.close();
  return linear && streamed;
}

let ok = true;
ok = (await phaseA({ name: 'week', channelCount: 16, hz: 5, hours: 24 * 7 })) && ok;
ok = (await phaseA({ name: 'fast', channelCount: 8, hz: 50, hours: 24 })) && ok;
ok = (await phaseB()) && ok;

rmSync(out, { recursive: true, force: true });
console.log(ok ? '\nOK' : '\nFAILED');
process.exit(ok ? 0 : 1);
