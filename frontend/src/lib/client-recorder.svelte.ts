// =============================================================================
//  client-recorder.svelte.ts — the recorder as the interface sees it (M14).
//
//  Everything that decides what a row says lives in client-recorder.ts, where a
//  test can reach it.  This file owns the parts that only exist in a browser:
//  the timer that drives the fixed-rate grid, the page lifecycle, the lock that
//  keeps a second tab from writing a duplicate archive, and the reactive status
//  the Dashboard indicator reads.
//
//  It also owns one promise the operator is entitled to: the recording does not
//  stop because they navigated to the Hardware page.  Nothing here is tied to a
//  view being mounted.
// =============================================================================

import { LocalHistoryDb, requestPersistence } from './local-history-db';
import {
  ClientRecorderCore, reconcileUnfinished,
  type RecorderChannel, type RecorderSnapshot, type StartOptions,
} from './client-recorder';
import {
  acquireRecorderOwnership, tabOwnerId, LEASE_HEARTBEAT_MS, type LockHandle,
} from './recorder-lock';
import type { ChannelFrame } from './live';
import type { LocalRateMode, LocalSession } from './local-history-types';

const BROADCAST_CHANNEL = 'lab-controller-recorder';

/** How often the fixed-rate grid is serviced.  Well under the fastest mode
 *  (5 Hz), because a timer that ticks exactly on the interval drifts. */
const TICK_MS = 100;

/**
 * A tab that comes back after being frozen has to admit it was gone.  Anything
 * longer than this between ticks means the operating system suspended us, and
 * the recording gets a PAGE_SUSPENDED / PAGE_RESUMED pair around the hole —
 * the tablet-in-a-drawer case §21.4 exists for.
 */
const SUSPEND_GAP_MS = 5000;

export interface RecorderStatus extends RecorderSnapshot {
  /** True when this tab may record.  False means another tab owns it. */
  owned: boolean;
  ownedElsewhere: boolean;
  available: boolean;
  unavailableReason: string;
}

export class ClientRecorder {
  status = $state<RecorderStatus>({
    active: false, sessionId: '', name: '', channels: 0, rows: 0, pendingRows: 0,
    droppedRows: 0, gaps: 0, bytes: 0, startedClientEpochMs: 0,
    rateMode: '1Hz', connected: true, state: 'COMPLETE',
    owned: false, ownedElsewhere: false, available: true, unavailableReason: '',
  });

  /** Sessions a previous page left open, waiting for the operator to decide. */
  interrupted = $state<LocalSession[]>([]);

  private db: LocalHistoryDb | null = null;
  private core: ClientRecorderCore | null = null;
  private lock: LockHandle | null = null;
  private timer: ReturnType<typeof setInterval> | null = null;
  private heartbeat: ReturnType<typeof setInterval> | null = null;
  private channel: BroadcastChannel | null = null;
  private readonly owner = tabOwnerId();
  private lastTickMs = 0;
  private controllerId = '';

  get database(): LocalHistoryDb | null { return this.db; }
  get recordingChannels(): RecorderChannel[] { return this.channels; }
  private channels: RecorderChannel[] = [];

  /** Channel handles the WebSocket subscription must include even when no
   *  widget on screen wants them. */
  get subscribedHandles(): number[] {
    return this.status.active ? this.channels.map((c) => c.handle) : [];
  }

  /**
   * Open the archive for this controller and report anything a previous page
   * left unfinished.  Safe to call again when the controller changes.
   */
  async attach(controllerId: string): Promise<void> {
    if (this.controllerId === controllerId && this.db) return;
    this.controllerId = controllerId;
    if (!LocalHistoryDb.available()) {
      this.status.available = false;
      this.status.unavailableReason =
        'this browser has no IndexedDB, so nothing can be recorded on this device';
      return;
    }
    try {
      this.db = await LocalHistoryDb.open();
      this.core = new ClientRecorderCore(this.db);
      this.status.available = true;
      this.status.unavailableReason = '';
      // A tab that died mid-recording left a session claiming to be RECORDING.
      // Say so; do not quietly adopt it.
      this.interrupted = await reconcileUnfinished(this.db, controllerId);
      this.listenToOtherTabs();
    } catch (error) {
      this.status.available = false;
      this.status.unavailableReason =
        error instanceof Error ? error.message : String(error);
    }
  }

  private listenToOtherTabs(): void {
    if (this.channel || typeof BroadcastChannel === 'undefined') return;
    this.channel = new BroadcastChannel(BROADCAST_CHANNEL);
    this.channel.onmessage = (event) => {
      const data = event.data as { controllerId?: string; active?: boolean;
                                   owner?: string } | null;
      if (!data || data.controllerId !== this.controllerId) return;
      if (data.owner === this.owner) return;
      // Another tab is recording this rig.  Show it; do not compete with it.
      this.status.ownedElsewhere = Boolean(data.active);
    };
  }

  private announce(): void {
    this.channel?.postMessage({
      controllerId: this.controllerId,
      owner: this.owner,
      active: this.status.active,
    });
  }

  // --- lifecycle -------------------------------------------------------------

  async start(options: Omit<StartOptions, 'controllerId'>): Promise<LocalSession> {
    if (!this.db || !this.core) throw new Error('the local archive is not open');
    // The lock is taken BEFORE the session row exists: a refused lock must not
    // leave a half-created recording behind.
    this.lock = await acquireRecorderOwnership(this.controllerId, this.owner);
    if (!this.lock) {
      this.status.ownedElsewhere = true;
      throw new Error('another tab is already recording this controller');
    }
    // Best effort, and explicitly not a guarantee — the dialog says so.
    void requestPersistence();

    const session = await this.core.start({ ...options, controllerId: this.controllerId });
    this.channels = options.channels.slice();
    this.status.owned = true;
    this.status.ownedElsewhere = false;
    this.sync();
    this.startTimers();
    this.announce();
    return session;
  }

  async stop(reason = 'stopped by the operator'): Promise<void> {
    if (!this.core) return;
    await this.core.stop(reason);
    this.stopTimers();
    this.lock?.release();
    this.lock = null;
    this.status.owned = false;
    this.channels = [];
    this.sync();
    this.announce();
  }

  private startTimers(): void {
    this.lastTickMs = Date.now();
    this.timer ??= setInterval(() => this.onTick(), TICK_MS);
    this.heartbeat ??= setInterval(() => this.lock?.heartbeat(), LEASE_HEARTBEAT_MS);
  }

  private stopTimers(): void {
    if (this.timer !== null) { clearInterval(this.timer); this.timer = null; }
    if (this.heartbeat !== null) { clearInterval(this.heartbeat); this.heartbeat = null; }
  }

  private onTick(): void {
    const core = this.core;
    if (!core || !core.active) return;
    const now = Date.now();
    // Timers do not fire in a frozen tab.  A long silence between ticks is the
    // only evidence we get that the device slept, and it must reach the record.
    if (now - this.lastTickMs > SUSPEND_GAP_MS) {
      void (async () => {
        await core.noteSuspended();
        await core.noteResumed();
        this.sync();
      })();
    }
    this.lastTickMs = now;
    core.tick(now);
    this.sync();
  }

  private sync(): void {
    if (!this.core) return;
    const snapshot = this.core.snapshot();
    // Assigned field by field so the reactive object identity is stable and
    // the status bar does not re-create itself four times a second.
    Object.assign(this.status, snapshot);
  }

  // --- the data plane --------------------------------------------------------

  onFrame(frame: ChannelFrame): void {
    this.core?.onFrame(frame);
  }

  async onConnectionChanged(connected: boolean): Promise<void> {
    const core = this.core;
    if (!core) return;
    if (connected) await core.noteReconnected();
    else await core.noteDisconnected();
    this.sync();
  }

  async onConfigChanged(revision: number): Promise<void> {
    await this.core?.noteConfigChanged(revision);
  }

  async mark(label: string): Promise<void> {
    await this.core?.mark(label);
    this.sync();
  }

  /** Best effort only: `pagehide` may be the last code that ever runs, and it
   *  may not run at all.  §14 — never build a guarantee on it. */
  flushBeforeUnload(): void {
    void this.core?.flush();
  }

  async refreshInterrupted(): Promise<void> {
    if (!this.db) return;
    this.interrupted = (await this.db.listSessions(this.controllerId))
      .filter((s) => s.state === 'INTERRUPTED' && !s.stopReason?.includes('acknowledged'));
  }

  dismissInterrupted(id: string): void {
    this.interrupted = this.interrupted.filter((s) => s.id !== id);
  }

  async listSessions(): Promise<LocalSession[]> {
    return this.db ? this.db.listSessions(this.controllerId) : [];
  }

  /** Refuses to remove the session currently being written — deleting the file
   *  you are writing is never what anyone meant. */
  async deleteSession(id: string): Promise<void> {
    if (!this.db) return;
    if (this.status.active && this.status.sessionId === id) {
      throw new Error('this recording is still running; stop it first');
    }
    await this.db.deleteSession(id);
  }
}

export function defaultRate(): LocalRateMode { return '1Hz'; }

export const recorder = new ClientRecorder();
