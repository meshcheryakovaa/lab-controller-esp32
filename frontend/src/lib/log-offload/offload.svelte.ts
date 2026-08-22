// =============================================================================
//  log-offload/offload.svelte.ts — the collector, as the interface sees it.
//
//  The state machine lives in SegmentCollector.ts where a test can reach it.
//  This owns the parts that only exist in a browser: the poll that drives it,
//  the Web Lock that stops a second tab acknowledging segments it does not
//  hold, and the reactive status the Logs page reads.
//
//  One promise it keeps that no view can: the collection does not stop because
//  the operator navigated away from Logs.  Nothing here is tied to a component
//  being mounted, because the consequence of pausing would be the controller's
//  flash filling up while somebody looks at the Dashboard.
// =============================================================================

import { api } from '../api';
import {
  acquireRecorderOwnership, tabOwnerId, LEASE_HEARTBEAT_MS, type LockHandle,
} from '../recorder-lock';
import { SegmentArchive } from './SegmentArchive';
import {
  SegmentCollector, emptyStatus,
  type CollectorStatus, type CollectorTransport,
} from './SegmentCollector';
import type { StoredEspSession } from './SegmentArchive';

/**
 * How often the queue is re-read when there is nothing to do.
 *
 * Two seconds, because REST is the source of truth: a WebSocket notification
 * makes collection prompt, and this makes it CERTAIN.  A lost frame must never
 * be able to strand a segment (§15).
 */
const IDLE_POLL_MS = 2000;

/** The collector id is stable for this browser profile so that a reload
 *  rejoins its own session rather than being refused as a stranger. */
const COLLECTOR_KEY = 'lc-collector-id';

function collectorId(): string {
  try {
    const existing = localStorage.getItem(COLLECTOR_KEY);
    if (existing) return existing;
    const fresh = `browser-${tabOwnerId().slice(4)}-${Date.now().toString(36)}`;
    localStorage.setItem(COLLECTOR_KEY, fresh);
    return fresh;
  } catch {
    // A browser that refuses storage still gets an id; it simply will not
    // survive a reload, and the controller will say the session is another
    // collector's — which is true, and better than pretending otherwise.
    return `browser-${tabOwnerId().slice(4)}`;
  }
}

const transport: CollectorTransport = {
  queue: (sessionId) => api.logSegments(sessionId),
  download: (sessionId, sequence) => api.downloadLogSegment(sessionId, sequence),
  acknowledge: (sessionId, sequence, proof) =>
    api.ackLogSegment(sessionId, sequence, proof),
};

export class OffloadService {
  status = $state<CollectorStatus>(emptyStatus());
  /** Set when another tab owns the collection: this one watches, and above all
   *  does not acknowledge (§18). */
  ownedElsewhere = $state(false);
  available = $state(true);
  unavailableReason = $state('');
  sessions = $state<StoredEspSession[]>([]);

  readonly id = collectorId();

  private archive: SegmentArchive | null = null;
  private collector: SegmentCollector | null = null;
  private lock: LockHandle | null = null;
  private timer: ReturnType<typeof setTimeout> | null = null;
  private heartbeat: ReturnType<typeof setInterval> | null = null;
  private controllerId = '';

  get store(): SegmentArchive | null { return this.archive; }

  async open(controllerId: string): Promise<void> {
    if (this.archive && this.controllerId === controllerId) return;
    this.controllerId = controllerId;
    if (!SegmentArchive.available()) {
      this.available = false;
      this.unavailableReason =
        'this browser has no IndexedDB, so segments cannot be collected here';
      return;
    }
    try {
      this.archive = await SegmentArchive.open();
      this.collector = new SegmentCollector(this.archive, transport);
      this.collector.onchange = (status) => { this.status = { ...status }; };
      this.available = true;
      this.unavailableReason = '';
      await this.refreshSessions();
    } catch (error) {
      this.available = false;
      this.unavailableReason =
        error instanceof Error ? error.message : String(error);
    }
  }

  async refreshSessions(): Promise<void> {
    if (!this.archive) return;
    this.sessions = await this.archive.listSessions(this.controllerId);
  }

  /** Enough room for two parts before a continuous run is allowed to start —
   *  a device that cannot hold two cannot keep up with a rotation (§17). */
  async hasRoom(segmentBytes: number): Promise<boolean> {
    return SegmentArchive.hasRoomFor(2, segmentBytes);
  }

  /**
   * Start collecting a session.  Takes the lock first: a refused lock must not
   * leave a half-attached collector that then acknowledges.
   */
  async attach(sessionId: string, meta?: Partial<StoredEspSession>): Promise<void> {
    if (!this.collector) return;
    if (this.status.sessionId === sessionId && this.collector.active) return;

    this.lock = await acquireRecorderOwnership(
      `log-offload/${this.controllerId}/${sessionId}`, this.id);
    if (!this.lock) {
      this.ownedElsewhere = true;
      return;
    }
    this.ownedElsewhere = false;
    await this.collector.attach(this.controllerId, sessionId, this.id, meta);
    this.heartbeat ??= setInterval(() => this.lock?.heartbeat(), LEASE_HEARTBEAT_MS);
    this.schedule(0);
  }

  detach(): void {
    this.collector?.detach();
    this.stopTimers();
    this.lock?.release();
    this.lock = null;
  }

  private stopTimers(): void {
    if (this.timer !== null) { clearTimeout(this.timer); this.timer = null; }
    if (this.heartbeat !== null) { clearInterval(this.heartbeat); this.heartbeat = null; }
  }

  private schedule(delayMs: number): void {
    if (this.timer !== null) clearTimeout(this.timer);
    this.timer = setTimeout(() => void this.tick(), delayMs);
  }

  private async tick(): Promise<void> {
    const collector = this.collector;
    if (!collector || !collector.active) { this.stopTimers(); return; }
    let more = false;
    try {
      more = await collector.pump();
    } catch (error) {
      // A bug here must not stop the loop: the controller's queue would fill
      // in silence, which is the outcome this whole feature exists to avoid.
      this.status = {
        ...this.status,
        lastError: error instanceof Error ? error.message : String(error),
      };
    }
    await this.refreshSessions();

    if (!collector.active) { this.stopTimers(); return; }
    // Straight on while there is work; the backoff only applies after a
    // failure, and the idle poll is the safety net under the WebSocket.
    if (more) { this.schedule(0); return; }
    const failed = this.status.state === 'WAITING_FOR_DEVICE'
                && this.status.attempts > 0;
    this.schedule(failed ? collector.retryDelayMs() : IDLE_POLL_MS);
  }

  /** A WebSocket said a part is ready — collect now rather than at the next
   *  poll.  Purely an accelerator; nothing depends on it arriving. */
  nudgeActive(): void {
    if (this.collector?.active) this.schedule(0);
  }

  async deleteSession(sessionId: string): Promise<void> {
    if (!this.archive) return;
    if (this.collector?.active && this.status.sessionId === sessionId) {
      throw new Error('this session is still being collected; stop it first');
    }
    await this.archive.deleteSession(this.controllerId, sessionId);
    await this.refreshSessions();
  }
}

export const offload = new OffloadService();
