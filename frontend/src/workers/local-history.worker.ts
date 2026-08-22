// =============================================================================
//  local-history.worker.ts — reading the local archive off the main thread.
//
//  Two jobs, both of which would otherwise freeze the interface on a tablet:
//  thinning a long recording down to what a chart can draw, and turning one
//  into CSV or ZIP.  Neither is hard; both walk millions of rows, and a busy
//  main thread on a controller UI is not a cosmetic problem — the master stop
//  button is on that thread.
//
//  Everything here streams.  The worker posts pieces as it produces them and
//  never holds a whole session, so a seven-day archive exports in the same
//  memory a five-minute one does.
// =============================================================================

import { LocalHistoryDb } from '../lib/local-history-db';
import { csvDocument, eventsCsv, readSeries, sessionZip } from '../lib/local-export';

export type WorkerRequest =
  | { id: number; op: 'series'; sessionId: string; channels: string[];
      from: number; to: number; buckets: number }
  | { id: number; op: 'csv'; sessionId: string; from?: number; to?: number }
  | { id: number; op: 'events-csv'; sessionId: string }
  | { id: number; op: 'zip'; sessionId: string };

export type WorkerResponse =
  | { id: number; kind: 'result'; result: unknown }
  | { id: number; kind: 'text'; piece: string }
  | { id: number; kind: 'bytes'; piece: Uint8Array }
  | { id: number; kind: 'done' }
  | { id: number; kind: 'error'; error: string };

let db: LocalHistoryDb | null = null;

async function database(): Promise<LocalHistoryDb> {
  db ??= await LocalHistoryDb.open();
  return db;
}

function post(message: WorkerResponse, transfer?: Transferable[]): void {
  (self as unknown as Worker).postMessage(message, transfer ?? []);
}

async function handle(request: WorkerRequest): Promise<void> {
  const store = await database();
  const session = await store.getSession(request.sessionId);
  if (!session) throw new Error(`no local session ${request.sessionId}`);

  switch (request.op) {
    case 'series': {
      const series = await readSeries(store, session, request.channels,
                                      request.from, request.to, request.buckets);
      post({ id: request.id, kind: 'result', result: series });
      return;
    }
    case 'csv': {
      for await (const piece of csvDocument(store, session, request.from, request.to)) {
        post({ id: request.id, kind: 'text', piece });
      }
      post({ id: request.id, kind: 'done' });
      return;
    }
    case 'events-csv': {
      const events = await store.listEvents(session.id);
      post({ id: request.id, kind: 'text', piece: eventsCsv(session, events) });
      post({ id: request.id, kind: 'done' });
      return;
    }
    case 'zip': {
      const events = await store.listEvents(session.id);
      const reader = sessionZip(store, session, events).getReader();
      for (;;) {
        const { done, value } = await reader.read();
        if (done) break;
        if (value) post({ id: request.id, kind: 'bytes', piece: value }, [value.buffer]);
      }
      post({ id: request.id, kind: 'done' });
      return;
    }
  }
}

self.onmessage = (event: MessageEvent<WorkerRequest>) => {
  const request = event.data;
  void handle(request).catch((error: unknown) => {
    post({
      id: request.id,
      kind: 'error',
      error: error instanceof Error ? error.message : String(error),
    });
  });
};
