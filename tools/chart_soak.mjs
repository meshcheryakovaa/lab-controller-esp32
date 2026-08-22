// =============================================================================
//  tools/chart_soak.mjs — the long-run test the browser could not survive.
//
//  Milestone 13, §8.2.  The failure being reproduced is not a crash: it is a
//  dashboard that has been open since yesterday morning and now takes a second
//  to answer a click, which every operator reads as "the ESP32 has hung".  So
//  the numbers that matter are the ones that used to grow without limit —
//  stored intervals, heap, and the time it takes to switch range.
//
//  Runs the real lib/chart-history.ts, not a model of it.
//
//  Usage:  node tools/chart_soak.mjs            (24 h x 8 ch x 5 Hz, then 1 h x 16 ch x 50 Hz)
//          node tools/chart_soak.mjs --quick     (one hour of each)
//
//  Run node with --expose-gc for a heap figure that is not mostly garbage.
// =============================================================================
import { pathToFileURL } from 'node:url';

// Imported straight from source: Node strips the type annotations, and a soak
// test that ran against a copy of the history would prove nothing about the one
// the dashboard uses.
const here = new URL('.', import.meta.url).pathname;
const { ChannelHistory, chartData } =
  await import(pathToFileURL(`${here}../frontend/src/lib/chart-history.ts`).href);

const quick = process.argv.includes('--quick');
/** Typed-array backing stores live outside the JS heap, and they are exactly
 *  what this history is made of — counting only heapUsed would report a ring
 *  buffer as free. */
const heap = () => {
  if (global.gc) global.gc();
  const usage = process.memoryUsage();
  return usage.heapUsed + usage.arrayBuffers;
};

function soak({ channels, hz, hours, width }) {
  const ran = quick ? Math.min(hours, 1) : hours;
  const name = `${channels} channels x ${hz} Hz x ${ran} h`;
  const seconds = ran * 3600;
  const samples = Math.round(seconds * hz);
  const step = 1000 / hz;
  // The rings are allocated once, at full size, before a single sample lands:
  // this is the whole cost, and it is paid up front rather than grown into.
  const empty = heap();
  const history = Array.from({ length: channels }, () => new ChannelHistory());
  const fixedMiB = (heap() - empty) / 1048576;

  const baseline = heap();
  const marks = [];
  const start = process.hrtime.bigint();

  for (let i = 0; i < samples; ++i) {
    const t = i * step;
    for (let c = 0; c < channels; ++c) {
      // A slow drift with one short excursion per channel per hour — the thing
      // that must still be visible after every level of thinning.
      const spike = i % Math.round(3600 * hz) === c * 37 ? 50 : 0;
      history[c].push(t, Math.sin((i / hz + c * 90) / 600) * 10 + spike);
    }
    // Sample the two numbers that used to grow, an hour of rig time apart.
    if (i > 0 && i % Math.round(3600 * hz) === 0) {
      marks.push({
        hour: Math.round(i / hz / 3600),
        intervals: history.reduce((n, h) => n + h.storedIntervals, 0),
        heapMiB: (heap() - baseline) / 1048576,
      });
    }
  }
  const feedMs = Number(process.hrtime.bigint() - start) / 1e6;

  const switchMs = {};
  for (const range of ['all', '10m', '5m', 'all']) {
    const at = process.hrtime.bigint();
    const data = chartData(history, range, width);
    switchMs[range] = Number(process.hrtime.bigint() - at) / 1e6;
    switchMs[`${range}.points`] = data.x.length;
  }

  const all = chartData(history, 'all', width);
  const extreme = Math.max(...all.series.flat().filter((v) => v !== null));

  console.log(`\n=== ${name} ===`);
  console.log(`  ${samples} samples x ${channels} channels in ${feedMs.toFixed(0)} ms`
            + ` (${((samples * channels) / feedMs / 1000).toFixed(1)} M samples/s)`);
  console.log(`  fixed allocation: ${fixedMiB.toFixed(2)} MiB for ${channels} channels`);
  for (const mark of marks) {
    console.log(`  after ${String(mark.hour).padStart(2)} h:`
              + ` ${String(mark.intervals).padStart(6)} intervals,`
              + ` heap +${mark.heapMiB.toFixed(1)} MiB`);
  }
  console.log(`  stored intervals now: ${history.reduce((n, h) => n + h.storedIntervals, 0)}`);
  for (const range of ['all', '10m', '5m']) {
    console.log(`  range "${range}": ${switchMs[range].toFixed(1)} ms,`
              + ` ${switchMs[`${range}.points`]} points`);
  }
  console.log(`  peak visible in "all": ${extreme.toFixed(1)} (planted 50 + drift)`);

  const plateau = marks.length < 3
    || marks[marks.length - 1].intervals <= marks[Math.floor(marks.length / 2)].intervals;
  const fast = Math.max(...['all', '10m', '5m'].map((r) => switchMs[r])) < 100;
  const spikeKept = extreme > 45;
  console.log(`  intervals plateaued: ${plateau ? 'yes' : 'NO'}`);
  console.log(`  every range under 100 ms: ${fast ? 'yes' : 'NO'}`);
  console.log(`  excursion survived thinning: ${spikeKept ? 'yes' : 'NO'}`);
  return plateau && fast && spikeKept;
}

let ok = true;
ok = soak({ channels: 8, hz: 5, hours: 24, width: 640 }) && ok;
ok = soak({ channels: 16, hz: 50, hours: 1, width: 640 }) && ok;

console.log(ok ? '\nOK' : '\nFAILED');
process.exit(ok ? 0 : 1);
