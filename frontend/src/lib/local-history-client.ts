// =============================================================================
//  local-history-client.ts — the main thread's side of the worker (M14).
//
//  Reading and exporting a local recording happen in a Web Worker, because the
//  main thread of this interface carries the master stop button and a UI that
//  janks while somebody exports a week of data is a safety problem, not a
//  cosmetic one.
//
//  Where a worker cannot be created — an old webview, a test — the same code
//  runs inline.  Slower and honest beats a feature that silently does nothing.
// =============================================================================

import { LocalHistoryDb } from './local-history-db';
import { csvDocument, eventsCsv, readSeries, sessionZip, type LocalSeries } from './local-export';
import type { WorkerRequest, WorkerResponse } from '../workers/local-history.worker';

let worker: Worker | null = null;
let workerBroken = false;
let nextId = 1;

function ensureWorker(): Worker | null {
  if (workerBroken) return null;
  if (worker) return worker;
  if (typeof Worker === 'undefined') { workerBroken = true; return null; }
  try {
    worker = new Worker(new URL('../workers/local-history.worker.ts', import.meta.url),
                        { type: 'module' });
    return worker;
  } catch {
    // Fall back rather than lose the feature.
    workerBroken = true;
    return null;
  }
}

/** A request without its id.  Written as a distributive Omit: a plain
 *  `Omit<WorkerRequest, 'id'>` over a union collapses to the shared fields and
 *  then rejects the ones that make each variant useful. */
type Unidentified<T> = T extends { id: number } ? Omit<T, 'id'> : never;

/** One request, one answer. */
function ask<T>(request: Unidentified<WorkerRequest>): Promise<T> | null {
  const w = ensureWorker();
  if (!w) return null;
  const id = nextId++;
  return new Promise<T>((resolve, reject) => {
    const onMessage = (event: MessageEvent<WorkerResponse>) => {
      const message = event.data;
      if (message.id !== id) return;
      if (message.kind === 'result') {
        w.removeEventListener('message', onMessage);
        resolve(message.result as T);
      } else if (message.kind === 'error') {
        w.removeEventListener('message', onMessage);
        reject(new Error(message.error));
      }
    };
    w.addEventListener('message', onMessage);
    w.postMessage({ ...request, id } as WorkerRequest);
  });
}

/** A streaming request: pieces arrive until `done`. */
function stream(request: Unidentified<WorkerRequest>): ReadableStream<Uint8Array> | null {
  const w = ensureWorker();
  if (!w) return null;
  const id = nextId++;
  const encoder = new TextEncoder();
  return new ReadableStream<Uint8Array>({
    start(controller) {
      const onMessage = (event: MessageEvent<WorkerResponse>) => {
        const message = event.data;
        if (message.id !== id) return;
        switch (message.kind) {
          case 'text': controller.enqueue(encoder.encode(message.piece)); break;
          case 'bytes': controller.enqueue(message.piece); break;
          case 'done':
            w.removeEventListener('message', onMessage);
            controller.close();
            break;
          case 'error':
            w.removeEventListener('message', onMessage);
            controller.error(new Error(message.error));
            break;
          default: break;
        }
      };
      w.addEventListener('message', onMessage);
      w.postMessage({ ...request, id } as WorkerRequest);
    },
  });
}

let inlineDb: LocalHistoryDb | null = null;
async function inline(): Promise<LocalHistoryDb> {
  inlineDb ??= await LocalHistoryDb.open();
  return inlineDb;
}

export async function readLocalSeries(sessionId: string, channels: string[],
                                      from: number, to: number,
                                      buckets: number): Promise<LocalSeries> {
  const viaWorker = ask<LocalSeries>({ op: 'series', sessionId, channels, from, to, buckets });
  if (viaWorker) return viaWorker;
  const db = await inline();
  const session = await db.getSession(sessionId);
  if (!session) throw new Error(`no local session ${sessionId}`);
  return readSeries(db, session, channels, from, to, buckets);
}

/** Text pieces to bytes, without ever concatenating the whole document.  Named
 *  export because M15's merged CSV goes through the same path. */
export function toByteStream(pieces: AsyncIterable<string>): ReadableStream<Uint8Array> {
  return textStream(pieces);
}

function textStream(pieces: AsyncIterable<string>): ReadableStream<Uint8Array> {
  const encoder = new TextEncoder();
  const iterator = pieces[Symbol.asyncIterator]();
  return new ReadableStream<Uint8Array>({
    async pull(controller) {
      const next = await iterator.next();
      if (next.done) { controller.close(); return; }
      controller.enqueue(encoder.encode(next.value));
    },
  });
}

export async function csvStream(sessionId: string): Promise<ReadableStream<Uint8Array>> {
  const viaWorker = stream({ op: 'csv', sessionId });
  if (viaWorker) return viaWorker;
  const db = await inline();
  const session = await db.getSession(sessionId);
  if (!session) throw new Error(`no local session ${sessionId}`);
  return textStream(csvDocument(db, session));
}

export async function zipStreamFor(sessionId: string): Promise<ReadableStream<Uint8Array>> {
  const viaWorker = stream({ op: 'zip', sessionId });
  if (viaWorker) return viaWorker;
  const db = await inline();
  const session = await db.getSession(sessionId);
  if (!session) throw new Error(`no local session ${sessionId}`);
  return sessionZip(db, session, await db.listEvents(sessionId));
}

export async function eventsCsvText(sessionId: string): Promise<string> {
  const db = await inline();
  const session = await db.getSession(sessionId);
  if (!session) throw new Error(`no local session ${sessionId}`);
  return eventsCsv(session, await db.listEvents(sessionId));
}

/**
 * Hand a stream to the browser as a download.
 *
 * `Response.blob()` is where the bytes finally land, and that is deliberate:
 * the browser owns that buffer and is free to back it with disk, whereas a
 * string we built would live in the tab's heap.  Our code never holds the whole
 * document at any point.
 */
export async function download(stream: ReadableStream<Uint8Array>,
                               filename: string, type: string): Promise<void> {
  const blob = await new Response(stream).blob();
  const url = URL.createObjectURL(new Blob([blob], { type }));
  const link = document.createElement('a');
  link.href = url;
  link.download = filename;
  document.body.appendChild(link);
  link.click();
  link.remove();
  // Revoked on the next turn: revoking immediately can beat the download.
  setTimeout(() => URL.revokeObjectURL(url), 10_000);
}
