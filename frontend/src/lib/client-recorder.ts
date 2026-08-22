// =============================================================================
//  client-recorder.ts — recording telemetry onto the viewing device (M14).
//
//  The core, with no Svelte, no DOM and no timers of its own: the reactive
//  wrapper (client-recorder.svelte.ts) owns the clock and the page lifecycle,
//  this owns the decisions.  Same split as chart-history.ts in M13, for the
//  same reason — the rules that must not be wrong are the ones a unit test can
//  reach.
//
//  THE RULE THIS FILE EXISTS TO KEEP: a recording may be incomplete, and must
//  never lie about being complete.  Every path that could invent a measurement
//  is closed here rather than in the UI:
//
//    * while the socket is down NO rows are written — the hole is the record;
//    * a channel that has stopped updating is written STALE, not GOOD;
//    * a channel never seen is absent, which `presentMask` distinguishes from 0;
//    * when the database falls behind, rows are DROPPED AND COUNTED, because an
//      unbounded queue turns a slow tablet into a lost recording.
// =============================================================================

import {
  CHUNK_DURATION_MS, MAX_PENDING_BYTES, MAX_PENDING_ROWS, MAX_QUEUED_ROWS,
  bytesPerRow, chunkBytes, makeSessionId, maskWordsPerRow, qualityCode,
  rateIntervalMs, setPresent,
  type LocalChunk, type LocalEvent, type LocalEventType, type LocalRateMode,
  type LocalSession, type LocalChannelDescriptor,
} from './local-history-types';
import { LocalHistoryDb, QuotaExceeded } from './local-history-db';
import type { ChannelFrame } from './live';
import type { ChannelQuality } from './types';

/** A channel as the recorder needs it: what it means, plus where its values
 *  arrive.  Handles are firmware slot indices and are NOT stored — a slot can
 *  belong to a different measurement tomorrow. */
export interface RecorderChannel extends LocalChannelDescriptor {
  handle: number;
}

export interface StartOptions {
  controllerId: string;
  dashboardKey: string;
  name: string;
  operator?: string;
  sample?: string;
  notes?: string;
  firmwareVersion: string;
  configRevision: number;
  rateMode: LocalRateMode;
  channels: RecorderChannel[];
  parentSessionId?: string;
}

export interface RecorderSnapshot {
  active: boolean;
  sessionId: string;
  name: string;
  channels: number;
  /** Rows safely on disk. */
  rows: number;
  /** Rows captured but not yet written.  Shown alongside `rows` because a
   *  block is only flushed once a minute: an indicator that read "0 rows" for
   *  the first minute of every recording looks exactly like one that is
   *  broken, and the operator would stop and restart it. */
  pendingRows: number;
  droppedRows: number;
  gaps: number;
  bytes: number;
  startedClientEpochMs: number;
  rateMode: LocalRateMode;
  connected: boolean;
  state: LocalSession['state'];
  stopReason?: string;
  lastError?: string;
}

/**
 * How long a channel may go without a fresh reading before its recorded quality
 * drops to STALE: two sampling intervals, and never less than three seconds.
 * The old number is still the best estimate available — but calling it GOOD
 * when nothing has confirmed it is the quiet lie this whole file is against.
 */
function stalenessLimitMs(intervalMs: number | null): number {
  return Math.max(3000, (intervalMs ?? 1000) * 2);
}

/** Backpressure is reported once per this window, not once per dropped row. */
const BACKPRESSURE_REPORT_MS = 10_000;

/** A device clock that jumps backwards by more than this is a reboot, not a
 *  frame that overtook another (same tolerance chart-history.ts uses). */
const DEVICE_ROLLBACK_TOLERANCE_MS = 1000;

// -----------------------------------------------------------------------------
//  The block being filled
// -----------------------------------------------------------------------------

/**
 * Rows accumulate here and leave as one columnar block.  Growth is by doubling
 * and stops dead at MAX_QUEUED_ROWS: the ceiling is the point of the class.
 */
export class ChunkBuilder {
  rows = 0;
  startEpochMs = 0;
  endEpochMs = 0;

  private capacity: number;
  private readonly maskWords: number;
  private clientEpochMs: Float64Array;
  private deviceMs: Float64Array;
  private values: Float32Array;
  private quality: Uint8Array;
  private presentMask: Uint32Array;

  constructor(readonly channelCount: number, initialRows = 64) {
    this.capacity = Math.max(1, initialRows);
    this.maskWords = maskWordsPerRow(channelCount);
    this.clientEpochMs = new Float64Array(this.capacity);
    this.deviceMs = new Float64Array(this.capacity);
    this.values = new Float32Array(this.capacity * channelCount);
    this.quality = new Uint8Array(this.capacity * channelCount);
    this.presentMask = new Uint32Array(this.capacity * this.maskWords);
  }

  get full(): boolean {
    return this.rows >= MAX_QUEUED_ROWS;
  }

  get bytes(): number {
    return this.rows * bytesPerRow(this.channelCount);
  }

  /** Whether this block should be handed to the database now. */
  shouldFlush(nowMs: number): boolean {
    if (this.rows === 0) return false;
    return this.rows >= MAX_PENDING_ROWS
        || this.bytes >= MAX_PENDING_BYTES
        || nowMs - this.startEpochMs >= CHUNK_DURATION_MS;
  }

  private grow(): void {
    const capacity = Math.min(MAX_QUEUED_ROWS, this.capacity * 2);
    if (capacity === this.capacity) return;
    const clientEpochMs = new Float64Array(capacity);
    const deviceMs = new Float64Array(capacity);
    const values = new Float32Array(capacity * this.channelCount);
    const quality = new Uint8Array(capacity * this.channelCount);
    const presentMask = new Uint32Array(capacity * this.maskWords);
    clientEpochMs.set(this.clientEpochMs);
    deviceMs.set(this.deviceMs);
    values.set(this.values);
    quality.set(this.quality);
    presentMask.set(this.presentMask);
    this.capacity = capacity;
    this.clientEpochMs = clientEpochMs;
    this.deviceMs = deviceMs;
    this.values = values;
    this.quality = quality;
    this.presentMask = presentMask;
  }

  /** Returns false when the block is at its hard ceiling — the caller must then
   *  count the row as dropped rather than making room. */
  append(clientEpochMs: number, deviceMs: number,
         value: (channel: number) => number | undefined,
         quality: (channel: number) => ChannelQuality | undefined): boolean {
    if (this.full) return false;
    if (this.rows === this.capacity) this.grow();
    const row = this.rows;
    if (row === 0) this.startEpochMs = clientEpochMs;
    this.endEpochMs = clientEpochMs;
    this.clientEpochMs[row] = clientEpochMs;
    this.deviceMs[row] = deviceMs;
    for (let c = 0; c < this.channelCount; ++c) {
      const v = value(c);
      const index = row * this.channelCount + c;
      this.quality[index] = qualityCode(quality(c));
      if (v === undefined || !Number.isFinite(v)) continue;
      this.values[index] = v;
      setPresent(this.presentMask, this.channelCount, row, c);
    }
    this.rows += 1;
    return true;
  }

  /** Cut the block down to exactly the rows it holds and hand it over.  The
   *  builder is finished afterwards; the caller makes a new one. */
  take(sessionId: string, sequence: number): LocalChunk {
    return {
      sessionId,
      sequence,
      startEpochMs: this.startEpochMs,
      endEpochMs: this.endEpochMs,
      rows: this.rows,
      clientEpochMs: this.clientEpochMs.slice(0, this.rows),
      deviceMs: this.deviceMs.slice(0, this.rows),
      values: this.values.slice(0, this.rows * this.channelCount),
      quality: this.quality.slice(0, this.rows * this.channelCount),
      presentMask: this.presentMask.slice(0, this.rows * this.maskWords),
    };
  }
}

// -----------------------------------------------------------------------------
//  The recorder
// -----------------------------------------------------------------------------

export class ClientRecorderCore {
  private session: LocalSession | null = null;
  private channels: RecorderChannel[] = [];
  private handleIndex = new Map<number, number>();

  private lastValue: (number | undefined)[] = [];
  private lastQuality: (ChannelQuality | undefined)[] = [];
  private lastUpdateMs: number[] = [];

  private builder: ChunkBuilder | null = null;
  private sequence = 0;
  private eventSequence = 0;
  private flushing = false;
  private flushRequested = false;
  private droppedRows = 0;
  private lastBackpressureReportMs = 0;

  private connected = true;
  private lastDeviceMs = Number.NEGATIVE_INFINITY;
  private nextRowAtMs = 0;
  private intervalMs: number | null = null;

  private lastError: string | undefined;
  private stopReason: string | undefined;
  private state: LocalSession['state'] = 'COMPLETE';

  constructor(private readonly db: LocalHistoryDb,
              private readonly now: () => number = () => Date.now()) {}

  get active(): boolean {
    return this.session !== null && this.session.state === 'RECORDING';
  }

  get sessionId(): string {
    return this.session?.id ?? '';
  }

  /** The channel keys the WebSocket subscription must keep alive, even when the
   *  operator has walked away from the Dashboard to the Hardware page. */
  get channelKeys(): string[] {
    return this.channels.map((c) => c.key);
  }

  get handles(): number[] {
    return this.channels.map((c) => c.handle);
  }

  snapshot(): RecorderSnapshot {
    const session = this.session;
    return {
      active: this.active,
      sessionId: session?.id ?? '',
      name: session?.name ?? '',
      channels: this.channels.length,
      rows: session?.rows ?? 0,
      pendingRows: this.builder?.rows ?? 0,
      droppedRows: this.droppedRows,
      gaps: session?.gaps ?? 0,
      bytes: session?.bytes ?? 0,
      startedClientEpochMs: session?.startedClientEpochMs ?? 0,
      rateMode: session?.rateMode ?? '1Hz',
      connected: this.connected,
      state: session?.state ?? this.state,
      stopReason: this.stopReason,
      lastError: this.lastError,
    };
  }

  async start(options: StartOptions): Promise<LocalSession> {
    if (this.active) throw new Error('a local recording is already running');
    if (options.channels.length === 0) {
      throw new Error('a recording with no channels would record nothing');
    }
    const startedClientEpochMs = this.now();
    const session: LocalSession = {
      id: makeSessionId(startedClientEpochMs),
      controllerId: options.controllerId,
      dashboardKey: options.dashboardKey,
      name: options.name,
      operator: options.operator ?? '',
      sample: options.sample ?? '',
      notes: options.notes ?? '',
      startedClientEpochMs,
      startedDeviceMs: 0,
      firmwareVersion: options.firmwareVersion,
      configRevision: options.configRevision,
      state: 'RECORDING',
      rateMode: options.rateMode,
      // Frozen here: renaming a channel or recalibrating it tomorrow must not
      // rewrite what today's rows claim to be.
      channels: options.channels.map((c) => ({
        key: c.key, name: c.name, unit: c.unit, quantity: c.quantity,
        precision: c.precision, calibrationId: c.calibrationId,
      })),
      rows: 0,
      droppedRows: 0,
      gaps: 0,
      bytes: 0,
      parentSessionId: options.parentSessionId,
    };

    this.session = session;
    this.channels = options.channels.slice();
    this.handleIndex = new Map(this.channels.map((c, i) => [c.handle, i]));
    this.lastValue = new Array(this.channels.length).fill(undefined);
    this.lastQuality = new Array(this.channels.length).fill(undefined);
    this.lastUpdateMs = new Array(this.channels.length).fill(Number.NEGATIVE_INFINITY);
    this.builder = new ChunkBuilder(this.channels.length);
    this.sequence = 0;
    this.eventSequence = 0;
    this.droppedRows = 0;
    this.flushRequested = false;
    this.lastError = undefined;
    this.stopReason = undefined;
    this.state = 'RECORDING';
    this.lastDeviceMs = Number.NEGATIVE_INFINITY;
    this.intervalMs = rateIntervalMs(options.rateMode);
    this.nextRowAtMs = startedClientEpochMs;

    await this.db.putSession(session);
    await this.db.putSettings({
      controllerId: options.controllerId,
      dashboardKey: options.dashboardKey,
      activeSessionId: session.id,
      lastChannels: this.channelKeys,
      lastRate: options.rateMode,
    });
    return session;
  }

  // --- the data plane --------------------------------------------------------

  /**
   * One delivered WebSocket batch.  In `every` mode this is also one row; at a
   * fixed rate it only updates what the next scheduled row will say.
   */
  onFrame(frame: ChannelFrame): void {
    if (!this.active) return;
    const nowMs = this.now();

    // A device clock that went backwards means the board restarted.  Recording
    // across it is fine — the rows carry client time — but the reader has to be
    // told, or a device-time axis will fold back on itself without explanation.
    if (frame.t < this.lastDeviceMs - DEVICE_ROLLBACK_TOLERANCE_MS) {
      void this.recordEvent('DEVICE_RESTARTED', undefined, frame.t);
    }
    this.lastDeviceMs = frame.t;
    if (this.session && this.session.startedDeviceMs === 0) {
      this.session.startedDeviceMs = frame.t;
    }

    for (const [handle, value] of frame.values) {
      const index = this.handleIndex.get(handle);
      if (index === undefined) continue;
      this.lastValue[index] = value;
      this.lastUpdateMs[index] = nowMs;
    }
    for (const [handle, quality] of frame.quality) {
      const index = this.handleIndex.get(handle);
      if (index === undefined) continue;
      this.lastQuality[index] = quality;
    }

    if (this.intervalMs === null) this.emitRow(nowMs, frame.t);
  }

  /**
   * Drive the fixed-rate grid.  Called by the wrapper's timer; deliberately not
   * self-scheduling so a test can run seven days in a loop.
   *
   * Nothing is written while the socket is down.  A three-hour outage must read
   * as three hours of nothing, not as three hours of the last value repeated.
   */
  tick(nowMs = this.now()): void {
    if (!this.active || this.intervalMs === null) return;
    if (!this.connected) {
      // Re-base the grid so reconnecting does not emit a burst of catch-up rows
      // stamped with times at which nothing was measured.
      this.nextRowAtMs = nowMs + this.intervalMs;
      return;
    }
    // Bounded catch-up: a tab that was throttled for an hour must not now write
    // an hour of fabricated grid points.
    let guard = 0;
    while (nowMs >= this.nextRowAtMs && guard < 64) {
      this.emitRow(this.nextRowAtMs, this.lastDeviceMs);
      this.nextRowAtMs += this.intervalMs;
      guard += 1;
    }
    if (nowMs >= this.nextRowAtMs) this.nextRowAtMs = nowMs + this.intervalMs;
  }

  private emitRow(clientEpochMs: number, deviceMs: number): void {
    const builder = this.builder;
    if (!builder) return;
    const limit = stalenessLimitMs(this.intervalMs);
    const appended = builder.append(
      clientEpochMs,
      Number.isFinite(deviceMs) ? deviceMs : 0,
      (c) => this.lastValue[c],
      (c) => {
        const seen = this.lastUpdateMs[c] ?? Number.NEGATIVE_INFINITY;
        if (!Number.isFinite(seen)) return undefined;   // never delivered
        const reported = this.lastQuality[c];
        if (clientEpochMs - seen > limit) return 'STALE';
        return reported ?? 'GOOD';
      });

    if (!appended) {
      // The hard ceiling.  Count it, say it once per window, and keep going:
      // dropping a few seconds beats losing the session to an OOM.
      this.droppedRows += 1;
      const nowMs = this.now();
      if (nowMs - this.lastBackpressureReportMs > BACKPRESSURE_REPORT_MS) {
        this.lastBackpressureReportMs = nowMs;
        void this.recordEvent('CLIENT_BACKPRESSURE',
          `${this.droppedRows} rows dropped: the local database is not keeping up`);
      }
      return;
    }
    if (builder.shouldFlush(clientEpochMs)) void this.flush();
  }

  // --- writing ---------------------------------------------------------------

  /**
   * Hand the current block to the database.
   *
   * A flush asked for while one is in flight is REMEMBERED, not dropped: the
   * writer loops until the builder is quiet.  Dropping it instead looked
   * harmless — the next row would ask again — but on a device where writes are
   * slower than rows arrive, "the next row" is the one being discarded at the
   * queue ceiling, and the recording would stall at 4096 rows with the disk
   * idle.  That is a lost run caused by bookkeeping, not by the hardware.
   */
  async flush(): Promise<void> {
    if (this.flushing) { this.flushRequested = true; return; }
    this.flushing = true;
    let failure: { reason: string; state: LocalSession['state'] } | null = null;
    try {
      do {
        this.flushRequested = false;
        const builder = this.builder;
        if (!builder || builder.rows === 0 || !this.session) break;
        const chunk = builder.take(this.session.id, this.sequence);
        this.builder = new ChunkBuilder(this.channels.length);
        this.sequence += 1;
        try {
          await this.db.appendChunk(chunk);
          this.session.rows += chunk.rows;
          this.session.bytes += chunkBytes(chunk);
          this.session.endedClientEpochMs = chunk.endEpochMs;
          this.session.endedDeviceMs = chunk.deviceMs[chunk.rows - 1];
        } catch (error) {
          // These rows are gone: they were taken out of the builder and the
          // write refused them.  Counted, not quietly forgotten.
          this.droppedRows += chunk.rows;
          failure = error instanceof QuotaExceeded
            ? { reason: 'local storage is full', state: 'FULL' }
            : {
                reason: error instanceof Error ? error.message : String(error),
                state: 'ERROR',
              };
          break;
        }
      } while (this.flushRequested);
    } finally {
      this.flushing = false;
    }
    if (!failure) return;
    if (failure.state === 'FULL') {
      await this.recordEvent('QUOTA_EXCEEDED',
        'the local archive is full; the ESP32 keeps measuring and logging');
    } else {
      this.lastError = failure.reason;
      await this.recordEvent('QUOTA_WARNING', failure.reason);
    }
    await this.stop(failure.reason, failure.state);
  }

  async stop(reason?: string, state: LocalSession['state'] = 'COMPLETE'): Promise<void> {
    const session = this.session;
    if (!session || session.state !== 'RECORDING') return;
    // Mark it finished BEFORE the final flush, so a flush failure cannot leave
    // the session claiming to still be recording.
    session.state = state;
    this.state = state;
    this.stopReason = reason;
    await this.flush();
    session.endedClientEpochMs = session.endedClientEpochMs ?? this.now();
    session.stopReason = reason;
    session.droppedRows = this.droppedRows;
    await this.db.patchSession(session.id, {
      state,
      stopReason: reason,
      endedClientEpochMs: session.endedClientEpochMs,
      endedDeviceMs: session.endedDeviceMs,
      droppedRows: this.droppedRows,
      gaps: session.gaps,
    });
    const settings = await this.db.getSettings(session.controllerId, session.dashboardKey);
    await this.db.putSettings({
      controllerId: session.controllerId,
      dashboardKey: session.dashboardKey,
      lastChannels: settings?.lastChannels ?? this.channelKeys,
      lastRate: settings?.lastRate ?? session.rateMode,
      softLimitBytes: settings?.softLimitBytes,
      activeSessionId: undefined,
    });
    this.session = null;
    this.builder = null;
  }

  // --- events ----------------------------------------------------------------

  async recordEvent(type: LocalEventType, label?: string,
                    deviceMs?: number): Promise<void> {
    const session = this.session;
    if (!session) return;
    const event: LocalEvent = {
      sessionId: session.id,
      sequence: this.eventSequence++,
      clientEpochMs: this.now(),
      deviceMs: deviceMs ?? (Number.isFinite(this.lastDeviceMs) ? this.lastDeviceMs : undefined),
      type,
      label,
    };
    try {
      await this.db.appendEvent(event);
    } catch (error) {
      // An event that cannot be written must not take the recording down.
      this.lastError = error instanceof Error ? error.message : String(error);
    }
  }

  /** An operator's note: "added solvent", "heater on". */
  async mark(label: string): Promise<void> {
    await this.recordEvent('MARK', label);
  }

  /**
   * The socket went away.  The current block is flushed so the data either side
   * of the hole is on disk, and the gap counter moves — the number the Local
   * data page shows so nobody reads a broken run as a continuous one.
   */
  async noteDisconnected(): Promise<void> {
    if (!this.active) { this.connected = false; return; }
    this.connected = false;
    if (this.session) this.session.gaps += 1;
    await this.recordEvent('WS_DISCONNECTED');
    await this.flush();
    if (this.session) {
      await this.db.patchSession(this.session.id, { gaps: this.session.gaps });
    }
  }

  async noteReconnected(): Promise<void> {
    this.connected = true;
    this.nextRowAtMs = this.now();
    if (!this.active) return;
    await this.recordEvent('WS_RECONNECTED');
  }

  /** The tab was frozen or hidden long enough that time passed unobserved. */
  async noteSuspended(): Promise<void> {
    if (!this.active) return;
    await this.recordEvent('PAGE_SUSPENDED');
    await this.flush();
  }

  async noteResumed(): Promise<void> {
    if (!this.active) return;
    this.nextRowAtMs = this.now();
    await this.recordEvent('PAGE_RESUMED');
  }

  async noteConfigChanged(revision: number): Promise<void> {
    if (!this.active) return;
    await this.recordEvent('CONFIG_CHANGED', `config_revision=${revision}`);
  }

  setConnected(connected: boolean): void {
    this.connected = connected;
  }
}

/**
 * Mark sessions that a closed tab left claiming to be RECORDING.
 *
 * Deliberately not "resume": the data is not appended to, because nothing can
 * prove that no process ran between the tab dying and the page coming back.
 * Continuing starts a NEW session that names the old one as its parent.
 */
export async function reconcileUnfinished(db: LocalHistoryDb,
                                          controllerId: string): Promise<LocalSession[]> {
  const stranded = await db.findUnfinished(controllerId);
  const marked: LocalSession[] = [];
  for (const session of stranded) {
    const patched = await db.patchSession(session.id, {
      state: 'INTERRUPTED',
      stopReason: 'the page closed while recording',
    });
    if (patched) marked.push(patched);
  }
  return marked;
}
