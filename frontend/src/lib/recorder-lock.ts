// =============================================================================
//  recorder-lock.ts — one recorder per controller, per browser (M14).
//
//  Two tabs open on the same rig must not both write.  They would produce two
//  overlapping archives of the same measurements, each incomplete in different
//  places, and the operator would have no way to tell which one to keep.
//
//  Web Locks is the right tool and is held for the whole recording.  Where it
//  is missing (older Safari, some embedded webviews) a localStorage lease with
//  a heartbeat does the same job less elegantly: a lease whose heartbeat has
//  stopped for longer than its lifetime is treated as abandoned, which is what
//  makes a crashed tab recoverable instead of permanently blocking.
//
//  No Svelte and no DOM assumptions: both backends are injected, so the
//  behaviour under contention is testable rather than hopeful.
// =============================================================================

export const LEASE_TTL_MS = 15_000;
export const LEASE_HEARTBEAT_MS = 5_000;

export interface LockHandle {
  readonly owner: string;
  release(): void;
  /** Keep the lease alive.  A no-op on the Web Locks path, where the browser
   *  holds the lock for as long as the promise is unresolved. */
  heartbeat(now?: number): void;
}

/** The slice of localStorage this needs, so a test can supply a plain Map. */
export interface LeaseStorage {
  getItem(key: string): string | null;
  setItem(key: string, value: string): void;
  removeItem(key: string): void;
}

interface Lease {
  owner: string;
  at: number;
}

export function leaseKey(controllerId: string): string {
  return `lc-recorder-lease:${controllerId}`;
}

/**
 * Take the lease if it is free or abandoned.
 *
 * "Abandoned" is the whole difficulty: a tab that crashed cannot release
 * anything, so the only evidence available is that its heartbeat stopped.  A
 * TTL three times the heartbeat interval means a merely busy tab is never
 * mistaken for a dead one.
 */
export function acquireLease(storage: LeaseStorage, controllerId: string,
                             owner: string, now: number,
                             ttlMs = LEASE_TTL_MS): LockHandle | null {
  const key = leaseKey(controllerId);
  const raw = storage.getItem(key);
  if (raw) {
    try {
      const held = JSON.parse(raw) as Lease;
      const alive = now - held.at < ttlMs;
      if (alive && held.owner !== owner) return null;
    } catch {
      // A corrupt lease is not a lock; fall through and take it.
    }
  }
  storage.setItem(key, JSON.stringify({ owner, at: now } satisfies Lease));
  return {
    owner,
    heartbeat(at = Date.now()) {
      const current = storage.getItem(key);
      if (current) {
        try {
          const held = JSON.parse(current) as Lease;
          // Somebody else took over while we were away; stop pretending.
          if (held.owner !== owner) return;
        } catch { /* corrupt: reclaim below */ }
      }
      storage.setItem(key, JSON.stringify({ owner, at } satisfies Lease));
    },
    release() {
      const current = storage.getItem(key);
      if (!current) return;
      try {
        const held = JSON.parse(current) as Lease;
        if (held.owner !== owner) return;   // never release another tab's lease
      } catch { /* corrupt: safe to clear */ }
      storage.removeItem(key);
    },
  };
}

export function leaseHolder(storage: LeaseStorage, controllerId: string,
                            now: number, ttlMs = LEASE_TTL_MS): string | null {
  const raw = storage.getItem(leaseKey(controllerId));
  if (!raw) return null;
  try {
    const held = JSON.parse(raw) as Lease;
    return now - held.at < ttlMs ? held.owner : null;
  } catch {
    return null;
  }
}

/**
 * Take ownership of recording for a controller, preferring Web Locks.
 *
 * Returns null when another tab already owns it — the caller then shows the
 * recording as running elsewhere rather than starting a second one.
 */
export async function acquireRecorderOwnership(
  controllerId: string,
  owner: string,
  options: { storage?: LeaseStorage; locks?: LockManager | null; now?: () => number } = {},
): Promise<LockHandle | null> {
  const now = options.now ?? (() => Date.now());
  const locks = options.locks !== undefined
    ? options.locks
    : (typeof navigator !== 'undefined' ? navigator.locks ?? null : null);

  if (locks) {
    const name = `lc-recorder:${controllerId}`;
    let release!: () => void;
    const held = new Promise<void>((resolve) => { release = resolve; });
    const granted = await new Promise<boolean>((resolve) => {
      void locks.request(name, { mode: 'exclusive', ifAvailable: true }, (lock) => {
        if (lock === null) { resolve(false); return undefined; }
        resolve(true);
        // Held until `release()` resolves this promise — that is how Web Locks
        // expresses "for the duration of the recording".
        return held;
      }).catch(() => resolve(false));
    });
    if (!granted) return null;
    return { owner, release, heartbeat() { /* the browser holds it */ } };
  }

  const storage = options.storage
    ?? (typeof localStorage !== 'undefined' ? localStorage : undefined);
  if (!storage) {
    // No Web Locks and no storage: a single-tab environment by construction.
    return { owner, release() {}, heartbeat() {} };
  }
  return acquireLease(storage, controllerId, owner, now());
}

/** A readable, unique-enough name for this tab; it appears in the "recording in
 *  another tab" notice, so it is a label, not just an id. */
export function tabOwnerId(random = Math.random): string {
  return `tab-${Math.floor(random() * 0xFFFFFF).toString(16).padStart(6, '0')}`;
}
