// =============================================================================
//  local-history-db.ts — the local archive on the viewing device (M14).
//
//  IndexedDB, wrapped thinly.  Two rules shape every method here:
//
//  1. NOTHING IS DELETED AUTOMATICALLY.  Scientific data is not cache.  When
//     the quota runs out the recording stops and says so; it does not make room
//     by discarding last night's run.  Deletion is always a user's decision.
//
//  2. NOTHING LOADS WHOLE.  A week of 16 channels is gigabytes.  Reads go
//     through cursors with a callback so the caller sees one chunk at a time,
//     and `readChunks()` exists only for the small ranges tests and the
//     exporter's own paging use.
//
//  The database is keyed by controllerId everywhere it matters: in access-point
//  mode every board answers on 192.168.4.1, so origin alone would pour two rigs
//  into one archive (see local-history-types.ts).
// =============================================================================

import {
  DB_NAME, DB_VERSION, chunkBytes,
  type LocalChunk, type LocalEvent, type LocalSession, type LocalSettings,
} from './local-history-types';

function wrap<T>(request: IDBRequest<T>): Promise<T> {
  return new Promise((resolve, reject) => {
    request.onsuccess = () => resolve(request.result);
    request.onerror = () => reject(request.error ?? new Error('IndexedDB request failed'));
  });
}

function finish(tx: IDBTransaction): Promise<void> {
  return new Promise((resolve, reject) => {
    tx.oncomplete = () => resolve();
    tx.onerror = () => reject(tx.error ?? new Error('IndexedDB transaction failed'));
    tx.onabort = () => reject(tx.error ?? new Error('IndexedDB transaction aborted'));
  });
}

/** Raised when the browser refuses a write for lack of room.  Distinct from a
 *  bug: the caller stops the recording and marks the session FULL. */
export class QuotaExceeded extends Error {
  constructor(message = 'the browser refused the write: local storage is full') {
    super(message);
    this.name = 'QuotaExceeded';
  }
}

function isQuotaError(error: unknown): boolean {
  return error instanceof DOMException
    && (error.name === 'QuotaExceededError' || error.code === 22);
}

export interface StorageUsage {
  usedBytes: number;
  quotaBytes: number;
  /** What THIS archive holds, which is not the same as what the origin uses. */
  sessionsBytes: number;
  persisted: boolean;
}

export class LocalHistoryDb {
  private constructor(private readonly db: IDBDatabase) {}

  static available(): boolean {
    return typeof indexedDB !== 'undefined' && indexedDB !== null;
  }

  static open(name: string = DB_NAME): Promise<LocalHistoryDb> {
    return new Promise((resolve, reject) => {
      const request = indexedDB.open(name, DB_VERSION);
      request.onupgradeneeded = () => {
        const db = request.result;
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
          // The range index: reading "12:40 to 12:55" must touch only those
          // chunks, never the whole session.
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
      };
      request.onsuccess = () => resolve(new LocalHistoryDb(request.result));
      request.onerror = () => reject(request.error ?? new Error('cannot open the local archive'));
    });
  }

  close(): void {
    this.db.close();
  }

  // --- sessions --------------------------------------------------------------

  async putSession(session: LocalSession): Promise<void> {
    const tx = this.db.transaction('sessions', 'readwrite');
    tx.objectStore('sessions').put(session);
    await finish(tx);
  }

  async getSession(id: string): Promise<LocalSession | undefined> {
    const tx = this.db.transaction('sessions', 'readonly');
    const found = await wrap(tx.objectStore('sessions').get(id) as IDBRequest<LocalSession>);
    await finish(tx);
    return found;
  }

  /**
   * Read-modify-write in ONE transaction.  The recorder updates row and byte
   * counts every few seconds while the UI may be renaming the same record; two
   * separate transactions would let one silently discard the other's field.
   */
  async patchSession(id: string,
                     patch: Partial<LocalSession>): Promise<LocalSession | undefined> {
    const tx = this.db.transaction('sessions', 'readwrite');
    const store = tx.objectStore('sessions');
    const current = await wrap(store.get(id) as IDBRequest<LocalSession>);
    if (!current) {
      await finish(tx);
      return undefined;
    }
    const merged = { ...current, ...patch };
    store.put(merged);
    await finish(tx);
    return merged;
  }

  /** Newest first.  Filtered by controller: another rig's runs are not this
   *  dashboard's business even when both answer on the same address. */
  async listSessions(controllerId?: string): Promise<LocalSession[]> {
    const tx = this.db.transaction('sessions', 'readonly');
    const all = await wrap(tx.objectStore('sessions').getAll() as IDBRequest<LocalSession[]>);
    await finish(tx);
    const filtered = controllerId
      ? all.filter((s) => s.controllerId === controllerId)
      : all;
    return filtered.sort((a, b) => b.startedClientEpochMs - a.startedClientEpochMs);
  }

  /** Sessions left RECORDING by a page that went away without stopping. */
  async findUnfinished(controllerId: string): Promise<LocalSession[]> {
    return (await this.listSessions(controllerId)).filter((s) => s.state === 'RECORDING');
  }

  async deleteSession(id: string): Promise<void> {
    const tx = this.db.transaction(['sessions', 'chunks', 'events'], 'readwrite');
    tx.objectStore('sessions').delete(id);
    for (const store of ['chunks', 'events'] as const) {
      const index = tx.objectStore(store).index('sessionId');
      const cursorRequest = index.openCursor(IDBKeyRange.only(id));
      cursorRequest.onsuccess = () => {
        const cursor = cursorRequest.result;
        if (!cursor) return;
        cursor.delete();
        cursor.continue();
      };
    }
    await finish(tx);
  }

  // --- chunks ----------------------------------------------------------------

  /**
   * Append one block and bill it to the session in the SAME transaction, so a
   * failed write can never leave a session claiming rows it does not have.
   */
  async appendChunk(chunk: LocalChunk): Promise<number> {
    const tx = this.db.transaction(['chunks', 'sessions'], 'readwrite');
    try {
      tx.objectStore('chunks').put(chunk);
      const sessions = tx.objectStore('sessions');
      const session = await wrap(sessions.get(chunk.sessionId) as IDBRequest<LocalSession>);
      const bytes = chunkBytes(chunk);
      if (session) {
        sessions.put({
          ...session,
          rows: session.rows + chunk.rows,
          bytes: session.bytes + bytes,
          endedClientEpochMs: chunk.endEpochMs,
        });
      }
      await finish(tx);
      return bytes;
    } catch (error) {
      if (isQuotaError(error)) throw new QuotaExceeded();
      throw error;
    }
  }

  async chunkCount(sessionId: string): Promise<number> {
    const tx = this.db.transaction('chunks', 'readonly');
    const count = await wrap(
      tx.objectStore('chunks').index('sessionId').count(IDBKeyRange.only(sessionId)));
    await finish(tx);
    return count;
  }

  /**
   * Walk the chunks of a session in `sequence` order, one at a time.
   *
   * This is THE read path: the callback sees a single block, so a viewer or an
   * exporter never holds the whole recording.  `from`/`to` are client epoch
   * milliseconds and are applied to the chunk's span, so a range query reads
   * only the blocks that overlap it.
   */
  async eachChunk(sessionId: string,
                  visit: (chunk: LocalChunk) => void | Promise<void> | boolean,
                  from = Number.NEGATIVE_INFINITY,
                  to = Number.POSITIVE_INFINITY): Promise<void> {
    const tx = this.db.transaction('chunks', 'readonly');
    const index = tx.objectStore('chunks').index('sessionStart');
    // Lower bound is deliberately open: the chunk containing `from` starts
    // BEFORE it.  Filtering by end below drops the ones that miss entirely.
    const range = IDBKeyRange.bound([sessionId, -Infinity], [sessionId, to]);
    const request = index.openCursor(range);
    const pending: LocalChunk[] = [];
    await new Promise<void>((resolve, reject) => {
      request.onsuccess = () => {
        const cursor = request.result;
        if (!cursor) { resolve(); return; }
        const chunk = cursor.value as LocalChunk;
        if (chunk.endEpochMs >= from) pending.push(chunk);
        cursor.continue();
      };
      request.onerror = () => reject(request.error);
    });
    await finish(tx);
    // Visited outside the transaction: an async consumer (a worker hop, a
    // stream write) would otherwise let the transaction auto-close underneath.
    pending.sort((a, b) => a.sequence - b.sequence);
    for (const chunk of pending) {
      const stop = await visit(chunk);
      if (stop === true) break;
    }
  }

  /** Convenience for tests and small ranges.  Deliberately not used by the
   *  exporter or the viewer, which must stay streaming. */
  async readChunks(sessionId: string, from?: number, to?: number): Promise<LocalChunk[]> {
    const out: LocalChunk[] = [];
    await this.eachChunk(sessionId, (chunk) => { out.push(chunk); }, from, to);
    return out;
  }

  // --- events ----------------------------------------------------------------

  async appendEvent(event: LocalEvent): Promise<void> {
    const tx = this.db.transaction('events', 'readwrite');
    tx.objectStore('events').put(event);
    await finish(tx);
  }

  async listEvents(sessionId: string): Promise<LocalEvent[]> {
    const tx = this.db.transaction('events', 'readonly');
    const found = await wrap(
      tx.objectStore('events').index('sessionId')
        .getAll(IDBKeyRange.only(sessionId)) as IDBRequest<LocalEvent[]>);
    await finish(tx);
    return found.sort((a, b) => a.sequence - b.sequence);
  }

  // --- settings --------------------------------------------------------------

  async getSettings(controllerId: string,
                    dashboardKey: string): Promise<LocalSettings | undefined> {
    const tx = this.db.transaction('settings', 'readonly');
    const found = await wrap(
      tx.objectStore('settings').get([controllerId, dashboardKey]) as IDBRequest<LocalSettings>);
    await finish(tx);
    return found;
  }

  async putSettings(settings: LocalSettings): Promise<void> {
    const tx = this.db.transaction('settings', 'readwrite');
    tx.objectStore('settings').put(settings);
    await finish(tx);
  }

  // --- space -----------------------------------------------------------------

  /**
   * What the browser will admit to.  `estimate()` is an approximation by
   * specification and can be rounded hard for privacy, so the UI presents it as
   * "approximately" and never as a guarantee.
   */
  async usage(controllerId?: string): Promise<StorageUsage> {
    let usedBytes = 0;
    let quotaBytes = 0;
    let persisted = false;
    const storage = typeof navigator !== 'undefined' ? navigator.storage : undefined;
    if (storage?.estimate) {
      try {
        const estimate = await storage.estimate();
        usedBytes = estimate.usage ?? 0;
        quotaBytes = estimate.quota ?? 0;
      } catch {
        // A browser that refuses to say is not an error worth stopping for.
      }
    }
    if (storage?.persisted) {
      try { persisted = await storage.persisted(); } catch { persisted = false; }
    }
    const sessions = await this.listSessions(controllerId);
    const sessionsBytes = sessions.reduce((total, s) => total + s.bytes, 0);
    return { usedBytes, quotaBytes, sessionsBytes, persisted };
  }
}

/**
 * Ask the browser not to evict this archive.  Granted or not, it is never
 * treated as a guarantee — §4 of the milestone says so out loud, and the UI
 * repeats it: an experiment worth keeping gets exported.
 */
export async function requestPersistence(): Promise<boolean> {
  const storage = typeof navigator !== 'undefined' ? navigator.storage : undefined;
  if (!storage?.persist) return false;
  try {
    return await storage.persist();
  } catch {
    return false;
  }
}
