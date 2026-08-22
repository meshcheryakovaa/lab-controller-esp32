// =============================================================================
//  log-offload/SegmentCollector.ts — emptying the controller's queue (M15 §16).
//
//  One loop, one segment at a time, and one rule that shapes all of it:
//
//      ACKNOWLEDGE ONLY WHAT IS PROVEN AND COMMITTED.
//
//  An acknowledgement causes the controller to erase a CSV that holds
//  measurements.  So the order is download → verify size → verify checksum →
//  commit to IndexedDB → *then* acknowledge, and every failure short of that
//  last step leaves the file exactly where it is.  A dropped connection, a full
//  disk, a corrupted transfer and a closed tab all resolve to "the controller
//  still has it", which is the only safe direction to fail in.
//
//  Deliberately sequential.  Two parallel downloads would double peak memory
//  for no gain against a 240 MHz controller serving one file at a time, and
//  they would make "acknowledge in order" — the property that lets a gap be
//  detected — something to reason about instead of something to read.
//
//  No Svelte and no DOM: the transport and the archive are injected, so the
//  retry behaviour and the failure paths are testable without a browser.
// =============================================================================

import {
  SegmentArchive, StorageFull, segmentKey, sessionKey,
  type StoredEspSegment, type StoredEspSession,
} from './SegmentArchive';
import { verifySegment } from './crc32';

export type CollectorState =
  | 'IDLE'
  | 'STARTING'
  | 'RECEIVING'
  | 'VERIFYING'
  | 'SAVING'
  | 'ACKNOWLEDGING'
  | 'WAITING_FOR_DEVICE'
  | 'LOCAL_STORAGE_FULL'
  | 'ERROR'
  | 'COMPLETE';

/** One waiting part, as the controller describes it. */
export interface PendingSegment {
  sequence: number;
  bytes: number;
  rows: number;
  first_row: number;
  last_row: number;
  payload_crc32: string;
  state: string;
}

export interface SegmentQueue {
  session_id: string;
  state: string;
  mode: string;
  collector_id: string;
  segments_completed: number;
  segments_acked: number;
  acked_through: number;
  rows: number;
  dropped: number;
  active_segment?: number;
  active_bytes?: number;
  segment_bytes: number;
  pending: PendingSegment[];
  pending_bytes: number;
  writable_bytes: number;
}

/** Everything the collector needs from the outside world. */
export interface CollectorTransport {
  queue(sessionId: string): Promise<SegmentQueue>;
  download(sessionId: string, sequence: number): Promise<Uint8Array>;
  acknowledge(sessionId: string, sequence: number, proof: {
    collector_id: string; bytes: number; payload_crc32: string;
  }): Promise<{ acknowledged: boolean; already_acknowledged?: boolean }>;
}

/** Backoff after a failed transfer.  Capped rather than unbounded: a run that
 *  lasts all night must keep trying all night, not give up at 3 a.m. */
export const RETRY_DELAYS_MS = [1000, 2000, 5000, 10_000, 30_000];

/** Three mismatches on the same part is not bad luck; something is wrong that
 *  retrying will not fix, and grinding on would hide it. */
export const MAX_VERIFY_FAILURES = 3;

export interface CollectorStatus {
  state: CollectorState;
  sessionId: string;
  controllerId: string;
  pending: number;
  pendingBytes: number;
  collected: number;
  collectedBytes: number;
  activeSegment: number;
  activeBytes: number;
  segmentBytes: number;
  rows: number;
  dropped: number;
  writableBytes: number;
  lastError: string;
  /** Consecutive failures against the CURRENT segment, not the session. */
  attempts: number;
  deviceState: string;
}

export function emptyStatus(): CollectorStatus {
  return {
    state: 'IDLE', sessionId: '', controllerId: '', pending: 0, pendingBytes: 0,
    collected: 0, collectedBytes: 0, activeSegment: 0, activeBytes: 0,
    segmentBytes: 0, rows: 0, dropped: 0, writableBytes: 0, lastError: '',
    attempts: 0, deviceState: '',
  };
}

export class SegmentCollector {
  private readonly archive: SegmentArchive;
  private readonly transport: CollectorTransport;
  private readonly now: () => number;

  private controllerId = '';
  private sessionId = '';
  private collectorId = '';
  private running = false;
  private verifyFailures = 0;

  status: CollectorStatus = emptyStatus();
  /** Called whenever `status` changes, so a UI can be reactive without this
   *  file importing a framework. */
  onchange: (status: CollectorStatus) => void = () => {};

  constructor(archive: SegmentArchive, transport: CollectorTransport,
              now: () => number = () => Date.now()) {
    this.archive = archive;
    this.transport = transport;
    this.now = now;
  }

  get active(): boolean { return this.running; }

  private set(patch: Partial<CollectorStatus>): void {
    this.status = { ...this.status, ...patch };
    this.onchange(this.status);
  }

  /**
   * Begin collecting for a session.  Idempotent: attaching to the session that
   * is already being collected does nothing, which is what makes it safe to
   * call from an effect that re-runs.
   */
  async attach(controllerId: string, sessionId: string, collectorId: string,
               meta?: Partial<StoredEspSession>): Promise<void> {
    if (this.running && this.sessionId === sessionId) return;
    this.controllerId = controllerId;
    this.sessionId = sessionId;
    this.collectorId = collectorId;
    this.running = true;
    this.verifyFailures = 0;
    this.set({ ...emptyStatus(), state: 'STARTING', controllerId, sessionId });

    const existing = await this.archive.getSession(controllerId, sessionId);
    if (!existing) {
      await this.archive.putSession({
        key: sessionKey(controllerId, sessionId),
        controllerId, sessionId,
        name: meta?.name ?? sessionId,
        operator: meta?.operator ?? '',
        sample: meta?.sample ?? '',
        firmwareVersion: meta?.firmwareVersion ?? '',
        rateHz: meta?.rateHz ?? 0,
        channels: meta?.channels ?? 0,
        startedEpochMs: meta?.startedEpochMs ?? this.now(),
        state: 'RECORDING',
        rows: 0, dropped: 0,
        segmentsCollected: 0, bytesCollected: 0,
        contiguousThrough: 0,
        updatedEpochMs: this.now(),
      });
      await this.archive.appendEvent(controllerId, sessionId, 'STARTED');
    }
  }

  detach(): void {
    this.running = false;
    this.set({ state: 'IDLE' });
  }

  /**
   * One pass: read the queue, take the OLDEST waiting part, and try to move it.
   *
   * Oldest first is not a preference.  Acknowledging out of order would let the
   * controller's contiguous mark stall behind a part that never arrives, and
   * would make "everything up to N is safely here" impossible to say.
   *
   * Returns true when there is more to do straight away.
   */
  async pump(): Promise<boolean> {
    if (!this.running) return false;

    let queue: SegmentQueue;
    try {
      queue = await this.transport.queue(this.sessionId);
    } catch (error) {
      // The controller is unreachable.  Nothing is lost: it keeps recording
      // into its own queue, and this is a wait, not a failure.
      this.set({
        state: 'WAITING_FOR_DEVICE',
        lastError: error instanceof Error ? error.message : String(error),
      });
      return false;
    }

    const stored = await this.archive.getSession(this.controllerId, this.sessionId);
    this.set({
      pending: queue.pending.length,
      pendingBytes: queue.pending_bytes,
      collected: stored?.segmentsCollected ?? 0,
      collectedBytes: stored?.bytesCollected ?? 0,
      activeSegment: queue.active_segment ?? 0,
      activeBytes: queue.active_bytes ?? 0,
      segmentBytes: queue.segment_bytes,
      rows: queue.rows,
      dropped: queue.dropped,
      writableBytes: queue.writable_bytes,
      deviceState: queue.state,
    });
    // Kept current on every pass, not only when the state changes: the row
    // count is what the operator reads to see the run progressing, and one
    // frozen at zero looks exactly like a collection that has stalled.
    if (stored && (stored.state !== queue.state || stored.rows !== queue.rows
                || stored.dropped !== queue.dropped)) {
      await this.archive.putSession({ ...stored, state: queue.state,
                                      rows: queue.rows, dropped: queue.dropped,
                                      updatedEpochMs: this.now() });
    }

    if (queue.pending.length === 0) {
      // Nothing waiting.  COMPLETE only when the controller says the run is
      // over as well — an empty queue mid-run just means we are keeping up.
      const finished = queue.state === 'COMPLETE_OFFLOADED'
                    || queue.state === 'COMPLETE';
      this.set({ state: finished ? 'COMPLETE' : 'WAITING_FOR_DEVICE', lastError: '' });
      if (finished) this.running = false;
      return false;
    }

    const next = queue.pending.reduce((lowest, item) =>
      item.sequence < lowest.sequence ? item : lowest);
    return this.collect(next);
  }

  /** Move one part from the controller to this device, and only then say so. */
  private async collect(segment: PendingSegment): Promise<boolean> {
    // A part left over from an earlier attempt may already be here and correct.
    // Re-downloading it would cost a minute of a 240 MHz controller's time for
    // nothing — but a copy that does NOT verify is worse than none, so it is
    // checked before it is trusted.
    const existing = await this.archive.getSegment(
      this.controllerId, this.sessionId, segment.sequence);
    if (existing) {
      const bytes = new Uint8Array(await existing.blob.arrayBuffer());
      const check = verifySegment(bytes, segment.bytes, segment.payload_crc32);
      if (check.ok) return this.acknowledge(segment);
      await this.archive.deleteSegment(this.controllerId, this.sessionId,
                                       segment.sequence);
      await this.archive.appendEvent(this.controllerId, this.sessionId,
                                     'VERIFY_FAILED', segment.sequence,
                                     `local copy discarded: ${check.reason}`);
    }

    this.set({ state: 'RECEIVING', lastError: '' });
    let file: Uint8Array;
    try {
      file = await this.transport.download(this.sessionId, segment.sequence);
    } catch (error) {
      // Interrupted mid-transfer.  No acknowledgement, nothing written, and the
      // controller's copy is untouched — the next pass simply asks again.
      const reason = error instanceof Error ? error.message : String(error);
      await this.archive.appendEvent(this.controllerId, this.sessionId,
                                     'DOWNLOAD_FAILED', segment.sequence, reason);
      this.set({ state: 'WAITING_FOR_DEVICE', lastError: reason,
                 attempts: this.status.attempts + 1 });
      return false;
    }

    this.set({ state: 'VERIFYING' });
    const check = verifySegment(file, segment.bytes, segment.payload_crc32);
    if (!check.ok) {
      this.verifyFailures += 1;
      await this.archive.appendEvent(this.controllerId, this.sessionId,
                                     'VERIFY_FAILED', segment.sequence, check.reason);
      if (this.verifyFailures >= MAX_VERIFY_FAILURES) {
        // Three times is a fault, not a fluke.  Stop, keep the controller's
        // copy, and say what is wrong rather than retrying for ever.
        this.set({ state: 'ERROR', lastError:
          `segment ${segment.sequence} failed verification ${this.verifyFailures} times:`
          + ` ${check.reason}` });
        this.running = false;
        return false;
      }
      this.set({ state: 'WAITING_FOR_DEVICE', lastError: check.reason,
                 attempts: this.status.attempts + 1 });
      return true;   // one immediate retry, per §23
    }
    this.verifyFailures = 0;

    this.set({ state: 'SAVING' });
    const record: StoredEspSegment = {
      key: segmentKey(this.controllerId, this.sessionId, segment.sequence),
      controllerId: this.controllerId,
      sessionId: this.sessionId,
      sequence: segment.sequence,
      filename: `${this.sessionId}_p${String(segment.sequence).padStart(6, '0')}.csv`,
      // `file.slice()` rather than the view itself: TypeScript's Blob accepts
      // only an ArrayBuffer-backed view, and copying the bytes here is the
      // honest way to satisfy that rather than casting the difference away.
      blob: new Blob([file.slice().buffer], { type: 'text/csv' }),
      size: segment.bytes,
      rows: segment.rows,
      firstRow: segment.first_row,
      lastRow: segment.last_row,
      payloadCrc32: segment.payload_crc32,
      receivedEpochMs: this.now(),
    };
    try {
      await this.archive.putSegment(record);
    } catch (error) {
      if (error instanceof StorageFull) {
        // No acknowledgement: the controller keeps the part, and keeps
        // recording while its queue has room.  The interface asks the operator
        // to export and delete something.
        await this.archive.appendEvent(this.controllerId, this.sessionId,
                                       'STORAGE_FULL', segment.sequence);
        this.set({ state: 'LOCAL_STORAGE_FULL',
                   lastError: 'this device is out of room for another segment' });
        this.running = false;
        return false;
      }
      const reason = error instanceof Error ? error.message : String(error);
      this.set({ state: 'ERROR', lastError: reason });
      this.running = false;
      return false;
    }
    await this.archive.appendEvent(this.controllerId, this.sessionId,
                                   'SEGMENT_STORED', segment.sequence);

    return this.acknowledge(segment);
  }

  /** The only call that deletes anything, made only after a committed write. */
  private async acknowledge(segment: PendingSegment): Promise<boolean> {
    this.set({ state: 'ACKNOWLEDGING' });
    try {
      await this.transport.acknowledge(this.sessionId, segment.sequence, {
        collector_id: this.collectorId,
        bytes: segment.bytes,
        payload_crc32: segment.payload_crc32,
      });
    } catch (error) {
      // The file is here and safe; only the confirmation failed.  Retrying is
      // free because a repeated ACK is idempotent on the controller (§14.4).
      const reason = error instanceof Error ? error.message : String(error);
      this.set({ state: 'WAITING_FOR_DEVICE', lastError: reason,
                 attempts: this.status.attempts + 1 });
      return false;
    }
    await this.archive.markAcknowledged(this.controllerId, this.sessionId,
                                        segment.sequence, this.now());
    await this.archive.appendEvent(this.controllerId, this.sessionId,
                                   'SEGMENT_ACKED', segment.sequence);
    this.set({ attempts: 0, lastError: '' });
    return true;
  }

  /** Delay before the next attempt, growing to a cap. */
  retryDelayMs(): number {
    const index = Math.min(this.status.attempts, RETRY_DELAYS_MS.length - 1);
    return RETRY_DELAYS_MS[index]!;
  }
}
