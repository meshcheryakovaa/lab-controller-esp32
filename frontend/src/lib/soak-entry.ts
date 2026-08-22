// =============================================================================
//  soak-entry.ts — the surface tools/recorder_soak.mjs drives (M14).
//
//  One re-export module so the long-run test bundles the REAL recorder,
//  database and exporter rather than a transcription of them.  Nothing in the
//  application imports this; it exists so that "what the browser runs" and
//  "what the soak test measures" cannot drift apart.
// =============================================================================

export { ClientRecorderCore, ChunkBuilder } from './client-recorder';
export { LocalHistoryDb } from './local-history-db';
export { csvDocument, readSeries } from './local-export';
export { MAX_QUEUED_ROWS } from './local-history-types';
