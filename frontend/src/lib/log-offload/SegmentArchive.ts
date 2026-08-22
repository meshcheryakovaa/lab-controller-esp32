// =============================================================================
//  log-offload/SegmentArchive.ts — where offloaded segments land (M15 §17).
//
//  Same IndexedDB as Milestone 14, different object stores.  The two are kept
//  apart on purpose: an M14 recording is what THIS BROWSER sampled off the
//  telemetry socket, while a segment here is a file the CONTROLLER wrote and
//  then deleted on our word.  Mixing them would blur which of the two a
//  scientist is holding, and only one of them is the instrument's own record.
//
//  The transaction boundary is the whole point of this file.  A segment's blob
//  and its metadata are written together or not at all, and the acknowledgement
//  that lets the controller erase the original is sent only after that
//  transaction has COMMITTED.  Acknowledging on a promise instead of a commit
//  is how a browser crash turns into a hole in an experiment.
// =============================================================================

import { DB_NAME, DB_VERSION } from '../local-history-types';

export interface StoredEspSegment {
  /** controllerId/sessionId/sequence — unique across rigs sharing an origin. */
  key: string;
  controllerId: string;
  sessionId: string;
  sequence: number;
  filename: string;
  blob: Blob;
  size: number;
  rows: number;
  firstRow: number;
  lastRow: number;
  payloadCrc32: string;
  receivedEpochMs: number;
  acknowledgedEpochMs?: number;
}

export interface StoredEspSession {
  key: string;                 // controllerId/sessionId
  controllerId: string;
  sessionId: string;
  name: string;
  operator: string;
  sample: string;
  firmwareVersion: string;
  rateHz: number;
  channels: number;
  startedEpochMs: number;
  /** What the controller last told us about the run. */
  state: string;
  rows: number;
  dropped: number;
  segmentsCollected: number;
  bytesCollected: number;
  /** Highest sequence stored locally with no gap below it. */
  contiguousThrough: number;
  updatedEpochMs: number;
}

export type OffloadEventType =
  | 'STARTED' | 'SEGMENT_STORED' | 'SEGMENT_ACKED' | 'VERIFY_FAILED'
  | 'DOWNLOAD_FAILED' | 'STORAGE_FULL' | 'STOPPED' | 'TAKEN_OVER';

export interface OffloadEvent {
  key: string;                 // controllerId/sessionId/seq counter
  controllerId: string;
  sessionId: string;
  sequence: number;
  clientEpochMs: number;
  type: OffloadEventType;
  label?: string;
}

export function segmentKey(controllerId: string, sessionId: string,
                           sequence: number): string {
  // Zero-padded so the key order is the sequence order: IndexedDB compares
  // strings, and "10" before "9" would make "read them in order" a lie.
  return `${controllerId}/${sessionId}/${String(sequence).padStart(6, '0')}`;
}

export function sessionKey(controllerId: string, sessionId: string): string {
  return `${controllerId}/${sessionId}`;
}

function wrap<T>(request: IDBRequest<T>): Promise<T> {
  return new Promise((resolve, reject) => {
    request.onsuccess = () => resolve(request.result);
    request.onerror = () => reject(request.error ?? new Error('IndexedDB request failed'));
  });
}

function committed(tx: IDBTransaction): Promise<void> {
  return new Promise((resolve, reject) => {
    // `oncomplete`, not `onsuccess` of the last request: only this event means
    // the data is durable, and only after it may an ACK be sent.
    tx.oncomplete = () => resolve();
    tx.onerror = () => reject(tx.error ?? new Error('IndexedDB transaction failed'));
    tx.onabort = () => reject(tx.error ?? new Error('IndexedDB transaction aborted'));
  });
}

export class StorageFull extends Error {
  constructor(message = 'this device has no room left for another segment') {
    super(message);
    this.name = 'StorageFull';
  }
}

function isQuotaError(error: unknown): boolean {
  return error instanceof DOMException
    && (error.name === 'QuotaExceededError' || error.code === 22);
}

export class SegmentArchive {
  private readonly db: IDBDatabase;
  private eventSeq = 0;

  private constructor(db: IDBDatabase) {
    this.db = db;
  }

  static available(): boolean {
    return typeof indexedDB !== 'undefined' && indexedDB !== null;
  }

  static open(name: string = DB_NAME): Promise<SegmentArchive> {
    return new Promise((resolve, reject) => {
      const request = indexedDB.open(name, DB_VERSION);
      request.onupgradeneeded = () => {
        const db = request.result;
        // M14's stores are created here too when this happens to be the first
        // open: an upgrade handler that only knew about M15 would leave a
        // Dashboard recording unable to find its own tables.
        if (!db.objectStoreNames.contains('sessions')) {
          const sessions = db.createObjectStore('sessions', { keyPath: 'id' });
          sessions.createIndex('controllerId', 'controllerId');
          sessions.createIndex('startedClientEpochMs', 'startedClientEpochMs');
          sessions.createIndex('state', 'state');
        }
        if (!db.objectStoreNames.contains('chunks')) {
          const chunks = db.createObjectStore('chunks',
            { keyPath: ['sessionId', 'sequence'] });
          chunks.createIndex('sessionId', 'sessionId');
          chunks.createIndex('sessionStart', ['sessionId', 'startEpochMs']);
        }
        if (!db.objectStoreNames.contains('events')) {
          const events = db.createObjectStore('events',
            { keyPath: ['sessionId', 'sequence'] });
          events.createIndex('sessionId', 'sessionId');
        }
        if (!db.objectStoreNames.contains('settings')) {
          db.createObjectStore('settings', { keyPath: ['controllerId', 'dashboardKey'] });
        }
        // --- M15 ---------------------------------------------------------
        if (!db.objectStoreNames.contains('espLogSessions')) {
          const store = db.createObjectStore('espLogSessions', { keyPath: 'key' });
          store.createIndex('controller', ['controllerId', 'sessionId']);
        }
        if (!db.objectStoreNames.contains('espLogSegments')) {
          const store = db.createObjectStore('espLogSegments', { keyPath: 'key' });
          store.createIndex('session', ['controllerId', 'sessionId']);
          store.createIndex('sequence', ['controllerId', 'sessionId', 'sequence']);
          store.createIndex('acknowledged', ['sessionId', 'acknowledgedEpochMs']);
        }
        if (!db.objectStoreNames.contains('espLogEvents')) {
          const store = db.createObjectStore('espLogEvents', { keyPath: 'key' });
          store.createIndex('session', ['controllerId', 'sessionId']);
        }
      };
      request.onsuccess = () => resolve(new SegmentArchive(request.result));
      request.onerror = () =>
        reject(request.error ?? new Error('cannot open the local archive'));
    });
  }

  close(): void {
    this.db.close();
  }

  // --- sessions --------------------------------------------------------------

  async putSession(session: StoredEspSession): Promise<void> {
    const tx = this.db.transaction('espLogSessions', 'readwrite');
    tx.objectStore('espLogSessions').put(session);
    await committed(tx);
  }

  async getSession(controllerId: string,
                   sessionId: string): Promise<StoredEspSession | undefined> {
    const tx = this.db.transaction('espLogSessions', 'readonly');
    const found = await wrap(
      tx.objectStore('espLogSessions')
        .get(sessionKey(controllerId, sessionId)) as IDBRequest<StoredEspSession>);
    await committed(tx);
    return found;
  }

  async listSessions(controllerId?: string): Promise<StoredEspSession[]> {
    const tx = this.db.transaction('espLogSessions', 'readonly');
    const all = await wrap(
      tx.objectStore('espLogSessions').getAll() as IDBRequest<StoredEspSession[]>);
    await committed(tx);
    const filtered = controllerId
      ? all.filter((s) => s.controllerId === controllerId)
      : all;
    return filtered.sort((a, b) => b.startedEpochMs - a.startedEpochMs);
  }

  // --- segments --------------------------------------------------------------

  /**
   * Store one segment and update its session's running totals IN ONE
   * TRANSACTION.  The caller may acknowledge to the controller only after this
   * resolves — see the file header.
   */
  async putSegment(segment: StoredEspSegment): Promise<void> {
    const tx = this.db.transaction(['espLogSegments', 'espLogSessions'], 'readwrite');
    try {
      tx.objectStore('espLogSegments').put(segment);
      const sessions = tx.objectStore('espLogSessions');
      const key = sessionKey(segment.controllerId, segment.sessionId);
      const session = await wrap(sessions.get(key) as IDBRequest<StoredEspSession>);
      if (session) {
        sessions.put({
          ...session,
          segmentsCollected: session.segmentsCollected + 1,
          bytesCollected: session.bytesCollected + segment.size,
          contiguousThrough: segment.sequence === session.contiguousThrough + 1
            ? segment.sequence
            : session.contiguousThrough,
          updatedEpochMs: segment.receivedEpochMs,
        });
      }
      await committed(tx);
    } catch (error) {
      if (isQuotaError(error)) throw new StorageFull();
      throw error;
    }
  }

  async getSegment(controllerId: string, sessionId: string,
                   sequence: number): Promise<StoredEspSegment | undefined> {
    const tx = this.db.transaction('espLogSegments', 'readonly');
    const found = await wrap(
      tx.objectStore('espLogSegments')
        .get(segmentKey(controllerId, sessionId, sequence)) as IDBRequest<StoredEspSegment>);
    await committed(tx);
    return found;
  }

  /** Every stored segment of a session, in sequence order. */
  async listSegments(controllerId: string,
                     sessionId: string): Promise<StoredEspSegment[]> {
    const tx = this.db.transaction('espLogSegments', 'readonly');
    const found = await wrap(
      tx.objectStore('espLogSegments').index('session')
        .getAll(IDBKeyRange.only([controllerId, sessionId])) as IDBRequest<StoredEspSegment[]>);
    await committed(tx);
    return found.sort((a, b) => a.sequence - b.sequence);
  }

  async markAcknowledged(controllerId: string, sessionId: string,
                         sequence: number, at: number): Promise<void> {
    const tx = this.db.transaction('espLogSegments', 'readwrite');
    const store = tx.objectStore('espLogSegments');
    const key = segmentKey(controllerId, sessionId, sequence);
    const found = await wrap(store.get(key) as IDBRequest<StoredEspSegment>);
    if (found) store.put({ ...found, acknowledgedEpochMs: at });
    await committed(tx);
  }

  /** Remove a copy that failed verification.  Keeping a segment we know is
   *  wrong would let a later run mistake it for a good one and skip the
   *  re-download that would have fixed it. */
  async deleteSegment(controllerId: string, sessionId: string,
                      sequence: number): Promise<void> {
    const tx = this.db.transaction('espLogSegments', 'readwrite');
    tx.objectStore('espLogSegments')
      .delete(segmentKey(controllerId, sessionId, sequence));
    await committed(tx);
  }

  async deleteSession(controllerId: string, sessionId: string): Promise<void> {
    const tx = this.db.transaction(['espLogSessions', 'espLogSegments', 'espLogEvents'],
                                   'readwrite');
    tx.objectStore('espLogSessions').delete(sessionKey(controllerId, sessionId));
    for (const store of ['espLogSegments', 'espLogEvents'] as const) {
      const index = tx.objectStore(store).index('session');
      const cursorRequest = index.openCursor(IDBKeyRange.only([controllerId, sessionId]));
      cursorRequest.onsuccess = () => {
        const cursor = cursorRequest.result;
        if (!cursor) return;
        cursor.delete();
        cursor.continue();
      };
    }
    await committed(tx);
  }

  // --- events ----------------------------------------------------------------

  async appendEvent(controllerId: string, sessionId: string,
                    type: OffloadEventType, sequence = 0,
                    label?: string): Promise<void> {
    const event: OffloadEvent = {
      key: `${controllerId}/${sessionId}/${String(this.eventSeq++).padStart(6, '0')}`,
      controllerId, sessionId, sequence, type, label,
      clientEpochMs: Date.now(),
    };
    const tx = this.db.transaction('espLogEvents', 'readwrite');
    tx.objectStore('espLogEvents').put(event);
    await committed(tx);
  }

  async listEvents(controllerId: string, sessionId: string): Promise<OffloadEvent[]> {
    const tx = this.db.transaction('espLogEvents', 'readonly');
    const found = await wrap(
      tx.objectStore('espLogEvents').index('session')
        .getAll(IDBKeyRange.only([controllerId, sessionId])) as IDBRequest<OffloadEvent[]>);
    await committed(tx);
    return found.sort((a, b) => a.clientEpochMs - b.clientEpochMs);
  }

  // --- space -----------------------------------------------------------------

  /**
   * Whether there is room for at least `segments` more parts.
   *
   * Asked BEFORE a continuous run starts (§17): a device that cannot hold two
   * segments cannot keep up with a rotation, and finding that out at the first
   * handover means the controller's queue starts filling immediately.
   */
  static async hasRoomFor(segments: number, segmentBytes: number): Promise<boolean> {
    const storage = typeof navigator !== 'undefined' ? navigator.storage : undefined;
    if (!storage?.estimate) return true;   // a browser that will not say is not a no
    try {
      const estimate = await storage.estimate();
      const quota = estimate.quota ?? 0;
      const used = estimate.usage ?? 0;
      if (quota === 0) return true;
      return quota - used > segments * segmentBytes;
    } catch {
      return true;
    }
  }
}
