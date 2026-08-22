// =============================================================================
//  chart-history.ts — bounded chart history for the browser (M13).
//
//  The ESP32 has neither the RAM to keep chart history nor any reason to
//  re-send it, so the browser keeps it.  That makes the browser the component
//  that fails when a dashboard is left open for a week — which is exactly what
//  a lab instrument is for.  The old implementation kept 900 raw points per
//  channel in a plain array and called shift() on every sample; three minutes
//  of history, and a copy of the whole array 5 times a second per channel.
//
//  So: two fixed-size levels per channel, both pre-allocated, neither ever
//  growing.
//
//      sample -> open 1 s interval ----+--> RecentHistory  (600 s, detailed)
//                                      \-> OverviewHistory (whole session)
//
//  An interval keeps the FIRST, MINIMUM, MAXIMUM and LAST sample times and the
//  min/max/last values.  Expanded back out in time order that is at most three
//  points per second per channel whatever the sampling rate, and a 40 ms spike
//  survives every level of thinning because it IS the minimum or the maximum.
//  The overview halves itself and doubles its interval width when it fills, so
//  memory is constant and the time span is not.
//
//  Full-rate measurements are the DataLogger's CSV.  This is a view.
//
//  This file must not import Svelte, the DOM or uPlot: all of it is testable
//  as plain functions (chart-history.test.ts), and none of it needs a browser.
// =============================================================================

/** Which span a chart is showing.  Three, because a fourth is a menu. */
export type ChartRange = 'all' | '5m' | '10m';

export const RECENT_WINDOW_MS = 10 * 60 * 1000;
export const RECENT_BUCKET_MS = 1000;
export const RECENT_BUCKETS = 600;
export const OVERVIEW_MAX_POINTS = 2048;

/** Seconds shown by the '5m' range. */
export const SHORT_WINDOW_MS = 5 * 60 * 1000;

/**
 * How far time may step backwards before we call it a reboot rather than a
 * frame that overtook another.  Device time is `millis()`, so a restart lands
 * near zero; a REST snapshot arriving just after a WebSocket frame is a few
 * milliseconds, not a second.
 */
export const ROLLBACK_TOLERANCE_MS = 1000;

/**
 * A hole wider than this is drawn as a hole.  Joining the two ends with a
 * straight line would invent a measurement across a dropped connection, which
 * is the one thing a chart on an instrument must never do.
 *
 * It cannot be one number for every channel: a 50 Hz load cell and a sensor
 * that answers every five minutes are both normal, and a fixed threshold would
 * either draw the slow one as a dotted line of holes or hide a real outage on
 * the fast one.  So each series gets a threshold from its OWN typical spacing,
 * with a floor set by how coarse the stored level is.
 */
const GAP_MIN_MS = 3000;
const GAP_INTERVALS = 3;
const GAP_SPACINGS = 4;

export interface HistoryPoints {
  /** Ascending, no duplicates. */
  time: number[];
  value: number[];
}

export interface AlignedHistory {
  x: number[];
  /** One row per source, `null` where that source has nothing to say. */
  series: (number | null)[][];
}

const kEmptyPoints: HistoryPoints = { time: [], value: [] };

// -----------------------------------------------------------------------------
//  The ring
// -----------------------------------------------------------------------------

/**
 * A fixed-capacity ring of aggregated intervals, held as parallel typed arrays.
 * Nothing here allocates after construction and nothing moves on insert: the
 * oldest interval is overwritten and the head steps forward.
 */
class IntervalRing {
  readonly capacity: number;
  private head = 0;
  private count = 0;

  private readonly firstT: Float64Array;
  private readonly minT: Float64Array;
  private readonly maxT: Float64Array;
  private readonly lastT: Float64Array;
  private readonly minV: Float32Array;
  private readonly maxV: Float32Array;
  private readonly lastV: Float32Array;

  constructor(capacity: number) {
    this.capacity = capacity;
    this.firstT = new Float64Array(capacity);
    this.minT = new Float64Array(capacity);
    this.maxT = new Float64Array(capacity);
    this.lastT = new Float64Array(capacity);
    this.minV = new Float32Array(capacity);
    this.maxV = new Float32Array(capacity);
    this.lastV = new Float32Array(capacity);
  }

  get length(): number {
    return this.count;
  }

  clear(): void {
    this.head = 0;
    this.count = 0;
  }

  /** Physical slot of logical index `i` — the only place the wrap happens. */
  private at(i: number): number {
    const physical = this.head + i;
    return physical < this.capacity ? physical : physical - this.capacity;
  }

  firstTimeAt(i: number): number { return this.firstT[this.at(i)]!; }
  minTimeAt(i: number): number { return this.minT[this.at(i)]!; }
  maxTimeAt(i: number): number { return this.maxT[this.at(i)]!; }
  lastTimeAt(i: number): number { return this.lastT[this.at(i)]!; }
  minValueAt(i: number): number { return this.minV[this.at(i)]!; }
  maxValueAt(i: number): number { return this.maxV[this.at(i)]!; }
  lastValueAt(i: number): number { return this.lastV[this.at(i)]!; }

  push(firstT: number, minT: number, minV: number, maxT: number, maxV: number,
       lastT: number, lastV: number): void {
    let slot: number;
    if (this.count === this.capacity) {
      // Full: the oldest interval is where the newest goes, and the window
      // slides by one.  No copying, no shift().
      slot = this.head;
      this.head = this.head + 1 === this.capacity ? 0 : this.head + 1;
    } else {
      slot = this.at(this.count);
      this.count += 1;
    }
    this.firstT[slot] = firstT;
    this.minT[slot] = minT;
    this.minV[slot] = minV;
    this.maxT[slot] = maxT;
    this.maxV[slot] = maxV;
    this.lastT[slot] = lastT;
    this.lastV[slot] = lastV;
  }

  /** Fold a later interval into the newest one — used when the overview level
   *  has widened and several source intervals now share one slot. */
  mergeIntoTail(minT: number, minV: number, maxT: number, maxV: number,
                lastT: number, lastV: number): void {
    const slot = this.at(this.count - 1);
    if (minV < this.minV[slot]!) {
      this.minV[slot] = minV;
      this.minT[slot] = minT;
    }
    if (maxV > this.maxV[slot]!) {
      this.maxV[slot] = maxV;
      this.maxT[slot] = maxT;
    }
    this.lastT[slot] = lastT;
    this.lastV[slot] = lastV;
  }

  dropOldest(n: number): void {
    const drop = Math.min(n, this.count);
    this.head = (this.head + drop) % this.capacity;
    this.count -= drop;
  }

  /**
   * Halve the ring by merging neighbours pairwise.  Reads run ahead of writes
   * (destination i is never past source 2i), so this is safe in place and
   * allocates nothing.  The first and the last interval of the session both
   * survive: index 0 is always the left half of the first pair, and an odd
   * tail is carried across unmerged.
   */
  mergePairs(): void {
    const merged = Math.ceil(this.count / 2);
    for (let i = 0; i < merged; ++i) {
      const a = this.at(2 * i);
      const b = 2 * i + 1 < this.count ? this.at(2 * i + 1) : -1;
      const firstT = this.firstT[a]!;
      let minT = this.minT[a]!;
      let minV = this.minV[a]!;
      let maxT = this.maxT[a]!;
      let maxV = this.maxV[a]!;
      let lastT = this.lastT[a]!;
      let lastV = this.lastV[a]!;
      if (b >= 0) {
        if (this.minV[b]! < minV) { minV = this.minV[b]!; minT = this.minT[b]!; }
        if (this.maxV[b]! > maxV) { maxV = this.maxV[b]!; maxT = this.maxT[b]!; }
        lastT = this.lastT[b]!;
        lastV = this.lastV[b]!;
      }
      const dst = this.at(i);
      this.firstT[dst] = firstT;
      this.minT[dst] = minT;
      this.minV[dst] = minV;
      this.maxT[dst] = maxT;
      this.maxV[dst] = maxV;
      this.lastT[dst] = lastT;
      this.lastV[dst] = lastV;
    }
    this.count = merged;
  }
}

// -----------------------------------------------------------------------------
//  Expanding an interval back into points
// -----------------------------------------------------------------------------

// Reused so that expanding 2048 intervals allocates nothing.
const kOrderT = [0, 0, 0];
const kOrderV = [0, 0, 0];

/**
 * Emit min, max and last in their real time order — that is what keeps a spike
 * pointing the way it actually pointed.  Points at or before `from` are
 * dropped, and two readings that share an instant collapse to one with the
 * later value, so the caller never sees a duplicate timestamp.
 */
function emitAggregate(minT: number, minV: number, maxT: number, maxV: number,
                       lastT: number, lastV: number, from: number,
                       time: number[], value: number[]): void {
  kOrderT[0] = minT; kOrderV[0] = minV;
  kOrderT[1] = maxT; kOrderV[1] = maxV;
  kOrderT[2] = lastT; kOrderV[2] = lastV;
  // Stable insertion sort of three, so equal times keep min < max < last and
  // the dedup below lets the last one win.
  for (let i = 1; i < 3; ++i) {
    const t = kOrderT[i]!;
    const v = kOrderV[i]!;
    let j = i - 1;
    while (j >= 0 && kOrderT[j]! > t) {
      kOrderT[j + 1] = kOrderT[j]!;
      kOrderV[j + 1] = kOrderV[j]!;
      --j;
    }
    kOrderT[j + 1] = t;
    kOrderV[j + 1] = v;
  }
  for (let i = 0; i < 3; ++i) {
    const t = kOrderT[i]!;
    const v = kOrderV[i]!;
    if (t < from) continue;
    const n = time.length;
    if (n > 0 && time[n - 1] === t) {
      value[n - 1] = v;
      continue;
    }
    time.push(t);
    value.push(v);
  }
}

// -----------------------------------------------------------------------------
//  The overview level
// -----------------------------------------------------------------------------

/**
 * The whole session in a fixed number of intervals.  Every closed second is
 * offered to it; when the ring fills, neighbours merge pairwise and the
 * interval width doubles — 1 s, 2 s, 4 s, 8 s…  A day at 5 Hz ends up as ~64 s
 * intervals and the same 2048 slots it started with.
 */
class OverviewHistory {
  readonly ring = new IntervalRing(OVERVIEW_MAX_POINTS);
  /** Interval width, in RECENT_BUCKET_MS units. */
  private span = 1;
  /** How much of the newest interval is already filled, same units. */
  private tailFill = 0;

  get length(): number { return this.ring.length; }
  get spanMs(): number { return this.span * RECENT_BUCKET_MS; }

  clear(): void {
    this.ring.clear();
    this.span = 1;
    this.tailFill = 0;
  }

  append(firstT: number, minT: number, minV: number, maxT: number, maxV: number,
         lastT: number, lastV: number): void {
    if (this.ring.length > 0 && this.tailFill < this.span) {
      this.ring.mergeIntoTail(minT, minV, maxT, maxV, lastT, lastV);
      this.tailFill += 1;
      return;
    }
    if (this.ring.length === this.ring.capacity) this.compact();
    this.ring.push(firstT, minT, minV, maxT, maxV, lastT, lastV);
    this.tailFill = 1;
  }

  private compact(): void {
    const wasEven = this.ring.length % 2 === 0;
    const oldSpan = this.span;
    this.ring.mergePairs();
    this.span = oldSpan * 2;
    // An even count means the old tail was merged with a full interval before
    // it; an odd one means it crossed over untouched.
    this.tailFill = wasEven ? oldSpan + this.tailFill : this.tailFill;
  }
}

// -----------------------------------------------------------------------------
//  One channel
// -----------------------------------------------------------------------------

export class ChannelHistory {
  /**
   * Bumped by every accepted sample and by every reset.  A chart redraws when
   * this moves and not otherwise — which is why a rig sitting idle costs the
   * browser nothing at all.
   */
  revision = 0;

  private readonly recent = new IntervalRing(RECENT_BUCKETS);
  private readonly overview = new OverviewHistory();

  // The second currently being accumulated.  Held in fields rather than an
  // object so that the hot path allocates nothing.
  private openBucket = -1;
  private openFirstT = 0;
  private openMinT = 0;
  private openMinV = 0;
  private openMaxT = 0;
  private openMaxV = 0;
  private openLastT = 0;
  private openLastV = 0;

  private newest = Number.NEGATIVE_INFINITY;

  /** Device time of the newest accepted sample, or NaN when there is none. */
  get lastTime(): number {
    return this.newest === Number.NEGATIVE_INFINITY ? NaN : this.newest;
  }

  get empty(): boolean {
    return this.openBucket < 0 && this.recent.length === 0;
  }

  clear(): void {
    this.recent.clear();
    this.overview.clear();
    this.openBucket = -1;
    this.newest = Number.NEGATIVE_INFINITY;
    this.revision += 1;
  }

  /**
   * Take one reading.  Returns whether it was kept, so the caller can bump its
   * own revision only for readings that changed something.
   *
   * Rejected: anything not finite (a disconnected thermocouple reported as NaN
   * must not become a point at zero), and a sample that arrives out of order
   * behind one we already have.  A step backwards past the tolerance is a
   * reboot, not a late frame: the sequence before it cannot be plotted on the
   * same axis as the one after, so it is dropped rather than folded in.
   */
  push(t: number, v: number): boolean {
    if (!Number.isFinite(t) || !Number.isFinite(v)) return false;
    if (t < this.newest - ROLLBACK_TOLERANCE_MS) {
      this.clear();
    } else if (t < this.newest) {
      return false;
    }
    this.newest = t;

    const bucket = Math.floor(t / RECENT_BUCKET_MS);
    if (this.openBucket >= 0 && this.openBucket !== bucket) this.closeOpen();
    if (this.openBucket < 0) {
      this.openBucket = bucket;
      this.openFirstT = t;
      this.openMinT = t; this.openMinV = v;
      this.openMaxT = t; this.openMaxV = v;
      this.openLastT = t; this.openLastV = v;
    } else {
      if (v < this.openMinV) { this.openMinV = v; this.openMinT = t; }
      if (v > this.openMaxV) { this.openMaxV = v; this.openMaxT = t; }
      this.openLastT = t;
      this.openLastV = v;
    }
    this.revision += 1;
    return true;
  }

  private closeOpen(): void {
    this.recent.push(this.openFirstT, this.openMinT, this.openMinV,
                     this.openMaxT, this.openMaxV, this.openLastT, this.openLastV);
    this.overview.append(this.openFirstT, this.openMinT, this.openMinV,
                         this.openMaxT, this.openMaxV, this.openLastT, this.openLastV);
    this.openBucket = -1;
    // Sparse channels can hold 600 intervals spanning far more than ten
    // minutes; the recent level promises a duration, not a count.
    let drop = 0;
    const cutoff = this.newest - RECENT_WINDOW_MS;
    while (drop < this.recent.length && this.recent.lastTimeAt(drop) < cutoff) ++drop;
    if (drop > 0) this.recent.dropOldest(drop);
  }

  /** Width of one stored interval in the given range — what counts as a gap
   *  depends on how coarse the level being drawn is. */
  stepMs(range: ChartRange): number {
    return range === 'all' ? this.overview.spanMs : RECENT_BUCKET_MS;
  }

  /** The points to draw for a range, oldest first. */
  points(range: ChartRange): HistoryPoints {
    const time: number[] = [];
    const value: number[] = [];
    // A button that says "10 min" draws ten minutes.  The ring drops whole
    // intervals, so without this the leading one straddles the edge and the
    // chart shows a little more than it claims.
    const from = range === '5m' ? this.newest - SHORT_WINDOW_MS
               : range === '10m' ? this.newest - RECENT_WINDOW_MS
               : Number.NEGATIVE_INFINITY;
    const ring = range === 'all' ? this.overview.ring : this.recent;
    for (let i = 0; i < ring.length; ++i) {
      // Whole intervals that ended before the window are skipped without
      // looking at their three points.
      if (ring.lastTimeAt(i) < from) continue;
      emitAggregate(ring.minTimeAt(i), ring.minValueAt(i),
                    ring.maxTimeAt(i), ring.maxValueAt(i),
                    ring.lastTimeAt(i), ring.lastValueAt(i), from, time, value);
    }
    if (this.openBucket >= 0) {
      emitAggregate(this.openMinT, this.openMinV, this.openMaxT, this.openMaxV,
                    this.openLastT, this.openLastV, from, time, value);
    }
    return { time, value };
  }

  /** Stored intervals, for tests and diagnostics — the number that must stop
   *  growing however long the dashboard stays open. */
  get storedIntervals(): number {
    return this.recent.length + this.overview.length + (this.openBucket >= 0 ? 1 : 0);
  }
}

// -----------------------------------------------------------------------------
//  Building what uPlot is given
// -----------------------------------------------------------------------------

/**
 * Put several channels on one timebase.
 *
 * A k-way merge of already-sorted arrays, then one forward walk per series:
 * O(P·S), no binary search per point.  A series takes the last value at or
 * before the base time — never one from the future, which would draw a step
 * before the reading that caused it — and `null` when the newest thing it has
 * is older than `gapMs`, so a dropped connection reads as a dropped
 * connection.
 */
/**
 * What counts as a hole in THIS series: four of its own typical intervals, but
 * never less than the floor.  The median rather than the mean, because one
 * outage would otherwise raise the threshold enough to hide the next one.
 *
 * Fewer than three points says nothing about a rhythm, so nothing is called a
 * gap — a channel that has just produced its first reading is not in outage.
 */
export function gapThreshold(points: HistoryPoints, floorMs: number): number {
  const n = points.time.length;
  if (n < 3) return Number.POSITIVE_INFINITY;
  const spacing: number[] = [];
  for (let i = 1; i < n; ++i) spacing.push(points.time[i]! - points.time[i - 1]!);
  spacing.sort((a, b) => a - b);
  return Math.max(floorMs, spacing[spacing.length >> 1]! * GAP_SPACINGS);
}

export function alignSeries(sources: HistoryPoints[],
                            gapMs: number | number[]): AlignedHistory {
  const n = sources.length;
  const gapOf = (i: number) =>
    (Array.isArray(gapMs) ? gapMs[i] ?? Number.POSITIVE_INFINITY : gapMs);

  // Where each series stops having anything to say.  uPlot draws a break at a
  // null and a straight line at nothing, so a hole needs a COLUMN of its own:
  // with one series on the chart its own timestamps are the whole timebase, and
  // an outage between two of them would otherwise be ruled straight across.
  const breaks: number[][] = [];
  for (let i = 0; i < n; ++i) {
    const source = sources[i]!;
    const gap = gapOf(i);
    const marks: number[] = [];
    for (let j = 1; j < source.time.length; ++j) {
      const before = source.time[j - 1]!;
      const after = source.time[j]!;
      if (after - before <= gap) continue;
      // Just after the last reading, so the line ends where the data ended —
      // unless there is no room, in which case halfway.
      marks.push(before + 1 < after ? before + 1 : (before + after) / 2);
    }
    breaks.push(marks);
  }

  const x: number[] = [];
  const cursor = new Array<number>(n).fill(0);
  const breakCursor = new Array<number>(n).fill(0);

  for (;;) {
    let next = Number.POSITIVE_INFINITY;
    for (let i = 0; i < n; ++i) {
      const source = sources[i]!;
      const c = cursor[i]!;
      if (c < source.time.length && source.time[c]! < next) next = source.time[c]!;
      const b = breakCursor[i]!;
      if (b < breaks[i]!.length && breaks[i]![b]! < next) next = breaks[i]![b]!;
    }
    if (next === Number.POSITIVE_INFINITY) break;
    x.push(next);
    for (let i = 0; i < n; ++i) {
      const source = sources[i]!;
      while (cursor[i]! < source.time.length && source.time[cursor[i]!]! <= next) {
        cursor[i] = cursor[i]! + 1;
      }
      while (breakCursor[i]! < breaks[i]!.length && breaks[i]![breakCursor[i]!]! <= next) {
        breakCursor[i] = breakCursor[i]! + 1;
      }
    }
  }

  const series: (number | null)[][] = [];
  for (let i = 0; i < n; ++i) {
    const source = sources[i]!;
    const gap = gapOf(i);
    const marks = breaks[i]!;
    const row = new Array<number | null>(x.length);
    let read = 0;
    let mark = 0;
    let heldT = Number.NEGATIVE_INFINITY;
    let heldV = 0;
    for (let k = 0; k < x.length; ++k) {
      const t = x[k]!;
      while (read < source.time.length && source.time[read]! <= t) {
        heldT = source.time[read]!;
        heldV = source.value[read]!;
        ++read;
      }
      while (mark < marks.length && marks[mark]! < t) ++mark;
      if (mark < marks.length && marks[mark] === t) {
        row[k] = null;
        continue;
      }
      row[k] = heldT !== Number.NEGATIVE_INFINITY && t - heldT <= gap ? heldV : null;
    }
    series.push(row);
  }

  return { x, series };
}

/**
 * Thin an aligned block down to what the widget can actually show, keeping
 * every series' local minimum and maximum.  Dropping a spike here would undo
 * the whole point of storing min and max in the first place.
 *
 * The first and last columns always survive, so the drawn span never shrinks.
 */
export function decimate(data: AlignedHistory, limit: number): AlignedHistory {
  const n = data.x.length;
  if (n <= limit || n === 0) return data;

  const keep = new Uint8Array(n);
  let kept = 0;
  let buckets = Math.max(1, limit >> 1);
  for (;;) {
    keep.fill(0);
    for (let b = 0; b < buckets; ++b) {
      const start = Math.floor((b * n) / buckets);
      const end = Math.floor(((b + 1) * n) / buckets);
      if (end <= start) continue;
      // The last column of each bucket anchors the timebase, so the line keeps
      // its shape between the extremes.
      keep[end - 1] = 1;
      for (const row of data.series) {
        let lowAt = -1;
        let highAt = -1;
        let low = Number.POSITIVE_INFINITY;
        let high = Number.NEGATIVE_INFINITY;
        for (let i = start; i < end; ++i) {
          const value = row[i];
          if (value === null || value === undefined) continue;
          if (value < low) { low = value; lowAt = i; }
          if (value > high) { high = value; highAt = i; }
        }
        if (lowAt >= 0) keep[lowAt] = 1;
        if (highAt >= 0) keep[highAt] = 1;
      }
    }
    // Every edge of a hole survives thinning, whatever else does.  Losing the
    // null column would close the gap again and draw a line across an outage
    // that really happened — the exact lie the null was inserted to prevent.
    for (const row of data.series) {
      for (let i = 1; i < n; ++i) {
        if ((row[i] === null) !== (row[i - 1] === null)) {
          keep[i] = 1;
          keep[i - 1] = 1;
        }
      }
    }
    keep[0] = 1;
    keep[n - 1] = 1;
    kept = 0;
    for (let i = 0; i < n; ++i) kept += keep[i]!;
    if (kept <= limit || buckets <= 1) break;
    buckets >>= 1;
  }

  const index: number[] = [];
  const x: number[] = [];
  for (let i = 0; i < n; ++i) {
    if (!keep[i]) continue;
    index.push(i);
    x.push(data.x[i]!);
  }
  const series = data.series.map((row) => index.map((i) => row[i]!));
  return { x, series };
}

/** At most two points per horizontal pixel; below that the chart is a smear. */
export function displayLimit(width: number): number {
  return Math.max(300, Math.round(width * 2));
}

/**
 * The whole path from stored history to something uPlot can be handed.  Kept
 * here rather than in the component so that it can be tested without a DOM.
 */
export function chartData(histories: (ChannelHistory | undefined)[],
                          range: ChartRange, width: number): AlignedHistory {
  const sources = histories.map((h) => (h ? h.points(range) : kEmptyPoints));
  const gaps = sources.map((points, i) => {
    const step = histories[i]?.stepMs(range) ?? RECENT_BUCKET_MS;
    return gapThreshold(points, Math.max(GAP_MIN_MS, step * GAP_INTERVALS));
  });
  return decimate(alignSeries(sources, gaps), displayLimit(width));
}

/**
 * Which range a stored `window_s` means.  0 or absent is the whole session;
 * anything else is the nearer of the two live windows, because a chart that
 * silently ignored a stored setting would be worse than one that never had it.
 */
export function rangeFromWindowSeconds(seconds: unknown): ChartRange {
  if (typeof seconds !== 'number' || !Number.isFinite(seconds) || seconds <= 0) {
    return 'all';
  }
  return seconds <= (300 + 600) / 2 ? '5m' : '10m';
}

export function windowSecondsForRange(range: ChartRange): number {
  return range === 'all' ? 0 : range === '5m' ? 300 : 600;
}
