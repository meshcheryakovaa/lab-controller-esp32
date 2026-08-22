// =============================================================================
//  chart-history.test.ts — the bounded chart history (npm test).
//
//  §58: test first the things that fail QUIETLY.  A chart that thins its data
//  wrongly does not crash — it draws a plausible line with the spike missing,
//  or it grows until the tab dies eight hours in, which the operator reads as
//  "the controller hung".  Both are wrong answers delivered confidently, so
//  both are here: the limits, the extremes that must survive, and the holes
//  that must stay holes.
// =============================================================================

import { describe, expect, it } from 'vitest';
import {
  ChannelHistory, OVERVIEW_MAX_POINTS, RECENT_BUCKETS, RECENT_WINDOW_MS,
  alignSeries, chartData, decimate, displayLimit, gapThreshold,
  rangeFromWindowSeconds, windowSecondsForRange, type AlignedHistory,
} from './chart-history';

/** Feed `count` samples at `hz`, starting at `from` ms, from a generator. */
function feed(history: ChannelHistory, from: number, count: number, hz: number,
              value: (i: number, t: number) => number): number {
  const step = 1000 / hz;
  let t = from;
  for (let i = 0; i < count; ++i) {
    t = from + i * step;
    history.push(t, value(i, t));
  }
  return t;
}

function ascending(times: number[]): boolean {
  for (let i = 1; i < times.length; ++i) if (times[i]! <= times[i - 1]!) return false;
  return true;
}

describe('ChannelHistory limits', () => {
  it('stops growing however many samples arrive', () => {
    const history = new ChannelHistory();
    // A million readings — 5 Hz for 55 hours, or 50 Hz for five and a half.
    feed(history, 0, 1_000_000, 50, (i) => Math.sin(i / 500));
    expect(history.storedIntervals)
      .toBeLessThanOrEqual(RECENT_BUCKETS + OVERVIEW_MAX_POINTS + 1);
    expect(history.points('10m').time.length)
      .toBeLessThanOrEqual(3 * (RECENT_BUCKETS + 1));
    expect(history.points('all').time.length)
      .toBeLessThanOrEqual(3 * (OVERVIEW_MAX_POINTS + 1));
  });

  it('does not grow faster at 50 Hz than at 1 Hz', () => {
    const slow = new ChannelHistory();
    const fast = new ChannelHistory();
    feed(slow, 0, 3600, 1, (i) => i);
    feed(fast, 0, 3600 * 50, 50, (i) => i);
    expect(fast.storedIntervals).toBe(slow.storedIntervals);
  });

  it('returns points in chronological order after the ring has wrapped', () => {
    const history = new ChannelHistory();
    // Twice round the ten-minute ring.
    feed(history, 0, 20 * 60 * 5, 5, (i) => i % 17);
    const recent = history.points('10m');
    expect(recent.time.length).toBeGreaterThan(100);
    expect(ascending(recent.time)).toBe(true);
    expect(ascending(history.points('all').time)).toBe(true);
  });
});

describe('ranges', () => {
  it('the five-minute window holds nothing older than five minutes', () => {
    const history = new ChannelHistory();
    const last = feed(history, 0, 8 * 60 * 5, 5, (i) => i);
    const short = history.points('5m');
    expect(short.time.length).toBeGreaterThan(0);
    expect(short.time[0]!).toBeGreaterThanOrEqual(last - 5 * 60 * 1000);
    expect(short.time[short.time.length - 1]!).toBe(last);
  });

  it('the ten-minute window keeps what the five-minute one dropped', () => {
    const history = new ChannelHistory();
    const last = feed(history, 0, 12 * 60 * 5, 5, (i) => i);
    const long = history.points('10m');
    const short = history.points('5m');
    const between = long.time.filter(
      (t) => t < last - 5 * 60 * 1000 && t >= last - 10 * 60 * 1000);
    expect(between.length).toBeGreaterThan(0);
    expect(long.time.length).toBeGreaterThan(short.time.length);
    expect(long.time[0]!).toBeGreaterThanOrEqual(last - RECENT_WINDOW_MS);
  });

  it('all-time keeps the beginning, the end, the minimum and the maximum', () => {
    const history = new ChannelHistory();
    // Six hours at 5 Hz, with one clear minimum and one clear maximum planted
    // in the middle where every level of thinning would pass over them.
    const total = 6 * 3600 * 5;
    const lowAt = Math.floor(total * 0.31);
    const highAt = Math.floor(total * 0.72);
    const last = feed(history, 0, total, 5, (i) => {
      if (i === lowAt) return -999;
      if (i === highAt) return 999;
      return Math.sin(i / 1000);
    });
    const all = history.points('all');
    expect(all.time[0]!).toBeLessThan(1000);
    expect(all.time[all.time.length - 1]!).toBe(last);
    expect(Math.min(...all.value)).toBeCloseTo(-999, 3);
    expect(Math.max(...all.value)).toBeCloseTo(999, 3);
  });

  it('keeps a 40 ms spike that lasted one sample out of a million', () => {
    const history = new ChannelHistory();
    const spikeAt = 500_000;
    feed(history, 0, 1_000_000, 25, (i) => (i === spikeAt ? 42 : 0));
    const all = history.points('all');
    expect(Math.max(...all.value)).toBeCloseTo(42, 3);
    // And at the moment it happened, not somewhere convenient.
    const at = all.time[all.value.findIndex((v) => v > 41)]!;
    expect(Math.abs(at - spikeAt * 40)).toBeLessThan(1000);
  });
});

describe('sample acceptance', () => {
  it('collapses samples that share a timestamp, last one winning', () => {
    const history = new ChannelHistory();
    history.push(1000, 1);
    history.push(1000, 2);
    history.push(1000, 3);
    const points = history.points('10m');
    expect(points.time).toEqual([1000]);
    expect(points.value[0]!).toBeCloseTo(3, 5);
  });

  it('ignores NaN and Infinity rather than plotting them as zero', () => {
    const history = new ChannelHistory();
    expect(history.push(1000, Number.NaN)).toBe(false);
    expect(history.push(2000, Number.POSITIVE_INFINITY)).toBe(false);
    expect(history.push(3000, Number.NEGATIVE_INFINITY)).toBe(false);
    expect(history.push(Number.NaN, 5)).toBe(false);
    expect(history.points('all').time.length).toBe(0);
    expect(history.revision).toBe(0);
    expect(history.push(4000, 5)).toBe(true);
    expect(history.points('all').time).toEqual([4000]);
  });

  it('starts a new sequence when device time goes backwards', () => {
    const history = new ChannelHistory();
    feed(history, 100_000, 500, 5, (i) => i);
    expect(history.points('all').time.length).toBeGreaterThan(1);
    // The board rebooted: millis() is near zero again.  The two sequences
    // cannot share an axis, so the old one goes.
    history.push(5, 1);
    const after = history.points('all');
    expect(after.time).toEqual([5]);
  });

  it('drops a frame that merely overtook another, without wiping anything', () => {
    const history = new ChannelHistory();
    history.push(10_000, 1);
    expect(history.push(9_990, 2)).toBe(false);
    expect(history.points('all').time).toEqual([10_000]);
  });

  it('bumps the revision only for readings it kept', () => {
    const history = new ChannelHistory();
    history.push(1000, 1);
    const after = history.revision;
    history.push(1000, Number.NaN);
    history.push(500, 7);
    expect(history.revision).toBe(after);
  });
});

describe('alignSeries', () => {
  it('never carries a value backwards from the future', () => {
    const a = { time: [0, 1000, 2000], value: [1, 2, 3] };
    const b = { time: [1500], value: [9] };
    const { x, series } = alignSeries([a, b], 5000);
    expect(x).toEqual([0, 1000, 1500, 2000]);
    // b says nothing before 1500 and must not pretend otherwise.
    expect(series[1]!.slice(0, 2)).toEqual([null, null]);
    expect(series[1]!).toEqual([null, null, 9, 9]);
  });

  it('draws a hole where the connection dropped', () => {
    const before = { time: [0, 1000, 2000], value: [1, 1, 1] };
    const after = { time: [60_000, 61_000], value: [2, 2] };
    const merged = {
      time: [...before.time, ...after.time],
      value: [...before.value, ...after.value],
    };
    const other = { time: [0, 30_000, 60_000], value: [5, 5, 5] };
    const { x, series } = alignSeries([merged, other], 3000);
    // At 30 s the reconnecting channel has nothing recent: null, not a line
    // ruled across the outage.
    expect(series[0]![x.indexOf(30_000)]).toBeNull();
    expect(series[0]![x.indexOf(60_000)]).toBe(2);
  });

  it('takes a per-series threshold, so one slow channel is not all holes', () => {
    const fast = { time: [0, 200, 400, 600, 800], value: [1, 1, 1, 1, 1] };
    // Answers every ten seconds — normal for a slow sensor, and not an outage.
    const slow = { time: [0, 10_000, 20_000, 30_000], value: [2, 2, 2, 2] };
    const { x, series } = alignSeries(
      [fast, slow],
      [gapThreshold(fast, 3000), gapThreshold(slow, 3000)]);
    expect(series[1]![x.indexOf(20_000)]).toBe(2);
    // The fast one really has stopped by then.
    expect(series[0]![x.indexOf(20_000)]).toBeNull();
  });

  it('calls nothing a gap until a rhythm exists to break', () => {
    expect(gapThreshold({ time: [0], value: [1] }, 3000))
      .toBe(Number.POSITIVE_INFINITY);
    expect(gapThreshold({ time: [0, 100], value: [1, 1] }, 3000))
      .toBe(Number.POSITIVE_INFINITY);
  });

  it('is not talked out of a threshold by one long outage', () => {
    const points = { time: [0, 1000, 2000, 3000, 900_000, 901_000], value: [1, 1, 1, 1, 1, 1] };
    // The median ignores the outage; the mean would have swallowed it.
    expect(gapThreshold(points, 3000)).toBe(4000);
  });

  it('merges timebases without duplicating a shared instant', () => {
    const a = { time: [0, 10, 20], value: [1, 2, 3] };
    const b = { time: [10, 20, 30], value: [4, 5, 6] };
    const { x } = alignSeries([a, b], 1000);
    expect(x).toEqual([0, 10, 20, 30]);
  });
});

describe('decimate', () => {
  function ramp(n: number, spike: number): AlignedHistory {
    const x: number[] = [];
    const row: (number | null)[] = [];
    for (let i = 0; i < n; ++i) {
      x.push(i * 100);
      row.push(i === spike ? 1000 : Math.sin(i / 40));
    }
    return { x, series: [row] };
  }

  it('respects the limit and keeps the ends', () => {
    const data = ramp(5000, 1234);
    const thin = decimate(data, 600);
    expect(thin.x.length).toBeLessThanOrEqual(600);
    expect(thin.x[0]).toBe(data.x[0]);
    expect(thin.x[thin.x.length - 1]).toBe(data.x[data.x.length - 1]);
  });

  it('never drops a local maximum', () => {
    const thin = decimate(ramp(5000, 1234), 400);
    expect(Math.max(...(thin.series[0]! as number[]))).toBe(1000);
    expect(thin.x[thin.series[0]!.indexOf(1000)]).toBe(123_400);
  });

  it('keeps the extremes of every series, not just the first', () => {
    const n = 4000;
    const x: number[] = [];
    const a: (number | null)[] = [];
    const b: (number | null)[] = [];
    for (let i = 0; i < n; ++i) {
      x.push(i);
      a.push(i === 900 ? 500 : 0);
      b.push(i === 2500 ? -500 : 0);
    }
    const thin = decimate({ x, series: [a, b] }, 300);
    expect(thin.x.length).toBeLessThanOrEqual(300);
    expect(Math.max(...(thin.series[0]! as number[]))).toBe(500);
    expect(Math.min(...(thin.series[1]! as number[]))).toBe(-500);
  });

  it('does not close a hole while thinning', () => {
    const n = 6000;
    const x: number[] = [];
    const row: (number | null)[] = [];
    for (let i = 0; i < n; ++i) {
      x.push(i * 100);
      row.push(i > 2000 && i < 2100 ? null : Math.sin(i / 50));
    }
    const thin = decimate({ x, series: [row] }, 400);
    expect(thin.x.length).toBeLessThanOrEqual(400);
    expect(thin.series[0]!.some((v) => v === null)).toBe(true);
    // And the readings on either side of it are still there to end the line on.
    const holeAt = thin.series[0]!.indexOf(null);
    expect(thin.series[0]![holeAt - 1]).not.toBeNull();
    expect(thin.series[0]!.slice(holeAt).find((v) => v !== null)).not.toBeUndefined();
  });

  it('leaves data that already fits untouched', () => {
    const data = ramp(100, 10);
    expect(decimate(data, 300)).toBe(data);
  });
});

describe('chartData', () => {
  it('stays within two points per pixel for eight busy channels', () => {
    const histories: ChannelHistory[] = [];
    for (let c = 0; c < 8; ++c) {
      const history = new ChannelHistory();
      feed(history, 0, 24 * 3600 * 5, 5, (i) => Math.sin((i + c) / 700));
      histories.push(history);
    }
    for (const range of ['all', '10m', '5m'] as const) {
      const data = chartData(histories, range, 640);
      expect(data.x.length).toBeLessThanOrEqual(displayLimit(640));
      expect(data.series.length).toBe(8);
      for (const row of data.series) expect(row.length).toBe(data.x.length);
      expect(ascending(data.x)).toBe(true);
    }
  });

  it('leaves a hole where the socket dropped, and closes it on reconnect', () => {
    const history = new ChannelHistory();
    feed(history, 0, 300, 5, () => 1);
    // Ninety seconds off the air, then back.
    feed(history, 150_000, 300, 5, () => 2);
    const data = chartData([history], '10m', 600);
    const holes = data.series[0]!.filter((v) => v === null).length;
    expect(holes).toBeGreaterThan(0);
    expect(data.series[0]![0]).toBe(1);
    expect(data.series[0]![data.series[0]!.length - 1]).toBe(2);
  });

  it('draws nothing, rather than throwing, for a channel with no history', () => {
    const data = chartData([undefined, undefined], '10m', 400);
    expect(data.x).toEqual([]);
    expect(data.series.length).toBe(2);
  });
});

describe('rangeFromWindowSeconds', () => {
  it('maps the three stored values and rounds anything else', () => {
    expect(rangeFromWindowSeconds(0)).toBe('all');
    expect(rangeFromWindowSeconds(300)).toBe('5m');
    expect(rangeFromWindowSeconds(600)).toBe('10m');
    // Older dashboards stored whatever the number field allowed.
    expect(rangeFromWindowSeconds(120)).toBe('5m');
    expect(rangeFromWindowSeconds(3600)).toBe('10m');
    expect(rangeFromWindowSeconds(undefined)).toBe('all');
    expect(rangeFromWindowSeconds('300')).toBe('all');
    expect(rangeFromWindowSeconds(Number.NaN)).toBe('all');
  });

  it('round-trips the three ranges', () => {
    for (const range of ['all', '5m', '10m'] as const) {
      expect(rangeFromWindowSeconds(windowSecondsForRange(range))).toBe(range);
    }
  });
});
