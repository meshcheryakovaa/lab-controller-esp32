<script lang="ts">
  // ===========================================================================
  //  LocalDataView — what this device has recorded (M14 §15).
  //
  //  The page exists to make local storage legible, because it is the part of
  //  the system the operator cannot see any other way: the ESP32 does not know
  //  these sets exist, and no other computer will show them.  So every row says
  //  the things that decide whether a set is usable — rows, gaps, dropped rows,
  //  size and state — rather than just a name and a date.
  //
  //  Deleting takes a typed confirmation.  These are measurements, and there is
  //  no copy anywhere unless the operator made one.
  // ===========================================================================
  import { controller } from '../lib/state.svelte';
  import { recorder } from '../lib/client-recorder.svelte';
  import LocalSessionChart from '../components/LocalSessionChart.svelte';
  import { csvStream, download, zipStreamFor } from '../lib/local-history-client';
  import { formatBytes, formatDuration } from '../lib/format';
  import type { LocalEvent, LocalSession } from '../lib/local-history-types';

  let sessions = $state<LocalSession[]>([]);
  let selected = $state<LocalSession | null>(null);
  let events = $state<LocalEvent[]>([]);
  let usage = $state<{ used: number; quota: number; mine: number;
                       persisted: boolean } | null>(null);
  let busy = $state('');
  let error = $state('');
  let confirming = $state<LocalSession | null>(null);
  let confirmText = $state('');

  async function reload() {
    error = '';
    try {
      sessions = await recorder.listSessions();
      const db = recorder.database;
      if (db) {
        const u = await db.usage(controller.controllerId);
        usage = { used: u.usedBytes, quota: u.quotaBytes,
                  mine: u.sessionsBytes, persisted: u.persisted };
      }
    } catch (e) {
      error = e instanceof Error ? e.message : String(e);
    }
  }

  $effect(() => {
    // Re-reads whenever a recording starts or stops, so the list is never a
    // snapshot from before the run the operator just finished.
    void recorder.status.active;
    void reload();
  });

  async function open(session: LocalSession) {
    selected = session;
    events = [];
    const db = recorder.database;
    if (db) events = await db.listEvents(session.id);
  }

  async function exportCsv(session: LocalSession) {
    busy = session.id;
    error = '';
    try {
      await download(await csvStream(session.id), `${session.id}.csv`, 'text/csv');
    } catch (e) {
      error = e instanceof Error ? e.message : String(e);
    } finally {
      busy = '';
    }
  }

  async function exportZip(session: LocalSession) {
    busy = session.id;
    error = '';
    try {
      await download(await zipStreamFor(session.id), `${session.id}.zip`, 'application/zip');
    } catch (e) {
      error = e instanceof Error ? e.message : String(e);
    } finally {
      busy = '';
    }
  }

  async function remove(session: LocalSession) {
    error = '';
    try {
      await recorder.deleteSession(session.id);
      if (selected?.id === session.id) selected = null;
      confirming = null;
      confirmText = '';
      await reload();
    } catch (e) {
      error = e instanceof Error ? e.message : String(e);
    }
  }

  function duration(session: LocalSession): number {
    if (!session.endedClientEpochMs) return 0;
    return session.endedClientEpochMs - session.startedClientEpochMs;
  }

  function stateClass(state: LocalSession['state']): string {
    if (state === 'COMPLETE') return 'ok';
    if (state === 'RECORDING') return 'accent';
    return 'warn';
  }
</script>

<section class="page">
  <header class="head">
    <div>
      <!-- The frame already titles the page; repeating it here just pushed the
           table down. -->
      <p class="muted small">
        Recorded by this browser onto this device. The controller does not know
        these sets exist, and no other computer can see them — export anything
        worth keeping.
      </p>
    </div>
    {#if usage}
      <div class="usage small">
        <div><strong>{formatBytes(usage.mine)}</strong> <span class="muted">in local sets</span></div>
        <div class="muted">
          browser storage: {formatBytes(usage.used)} used of about
          {formatBytes(usage.quota)}
        </div>
        <div class="muted">
          {usage.persisted
            ? 'marked persistent — still not a guarantee'
            : 'not marked persistent: the browser may evict this'}
        </div>
      </div>
    {/if}
  </header>

  {#if !recorder.status.available}
    <p class="bad">{recorder.status.unavailableReason}</p>
  {/if}
  {#if error}<p class="bad">{error}</p>{/if}

  {#if recorder.interrupted.length > 0}
    <div class="panel interrupted">
      <strong>Recordings that were not closed</strong>
      <p class="muted small">
        The page went away while these were running. What was already written is
        intact; what happened afterwards was not recorded, and cannot be
        reconstructed — so they are marked INTERRUPTED rather than completed.
      </p>
      <ul>
        {#each recorder.interrupted as session (session.id)}
          <li>
            <span>{session.name}</span>
            <span class="muted numeric">
              last row {session.endedClientEpochMs
                ? new Date(session.endedClientEpochMs).toLocaleString()
                : 'none'}
            </span>
            <button type="button" onclick={() => recorder.dismissInterrupted(session.id)}>
              Acknowledge
            </button>
          </li>
        {/each}
      </ul>
    </div>
  {/if}

  {#if sessions.length === 0}
    <div class="panel empty">
      <p>Nothing recorded on this device yet.</p>
      <p class="muted small">
        Open the Dashboard and press <strong>Record on this device</strong>.
      </p>
    </div>
  {:else}
    <div class="panel">
      <table>
        <thead>
          <tr>
            <th>Name</th><th>Started</th><th>Duration</th><th>Channels</th>
            <th class="right">Rows</th><th class="right">Gaps</th>
            <th class="right">Size</th><th>State</th><th></th>
          </tr>
        </thead>
        <tbody>
          {#each sessions as session (session.id)}
            <tr class:selected={selected?.id === session.id}>
              <td>
                <button type="button" class="link" onclick={() => void open(session)}>
                  {session.name}
                </button>
                <div class="muted numeric tiny">{session.id}</div>
                {#if session.parentSessionId}
                  <div class="muted tiny">continues {session.parentSessionId}</div>
                {/if}
              </td>
              <td class="numeric tiny">
                {new Date(session.startedClientEpochMs).toLocaleString()}
              </td>
              <td class="numeric tiny">{formatDuration(duration(session))}</td>
              <td class="tiny">{session.channels.length}</td>
              <td class="right numeric tiny">
                {session.rows.toLocaleString()}
                {#if session.droppedRows > 0}
                  <span class="warn"> +{session.droppedRows} lost</span>
                {/if}
              </td>
              <td class="right numeric tiny" class:warn={session.gaps > 0}>{session.gaps}</td>
              <td class="right numeric tiny">{formatBytes(session.bytes)}</td>
              <td><span class={stateClass(session.state)}>{session.state}</span></td>
              <td class="actions">
                <button type="button" disabled={busy === session.id}
                        onclick={() => void exportCsv(session)}>CSV</button>
                <button type="button" disabled={busy === session.id}
                        onclick={() => void exportZip(session)}>ZIP</button>
                <button type="button" class="danger"
                        disabled={session.state === 'RECORDING'}
                        title={session.state === 'RECORDING'
                          ? 'stop the recording first'
                          : 'delete this set'}
                        onclick={() => { confirming = session; confirmText = ''; }}>
                  Delete
                </button>
              </td>
            </tr>
          {/each}
        </tbody>
      </table>
    </div>
  {/if}

  {#if confirming}
    <div class="panel danger-panel">
      <strong>Delete “{confirming.name}”?</strong>
      <p class="small">
        {confirming.rows.toLocaleString()} rows, {formatBytes(confirming.bytes)},
        recorded {new Date(confirming.startedClientEpochMs).toLocaleString()}.
        This is the only copy unless it was exported. Type the name to confirm.
      </p>
      <div class="row">
        <input type="text" bind:value={confirmText} placeholder={confirming.name} />
        <button type="button" class="danger"
                disabled={confirmText !== confirming.name}
                onclick={() => confirming && void remove(confirming)}>Delete</button>
        <button type="button" onclick={() => { confirming = null; confirmText = ''; }}>
          Cancel
        </button>
      </div>
    </div>
  {/if}

  {#if selected}
    <div class="panel">
      <header class="sub">
        <strong>{selected.name}</strong>
        <span class="muted small">
          {selected.controllerId} · dashboard {selected.dashboardKey} ·
          firmware {selected.firmwareVersion} · config {selected.configRevision} ·
          {selected.rateMode}
        </span>
        <button type="button" class="close" onclick={() => (selected = null)}>×</button>
      </header>
      <LocalSessionChart session={selected} {events} />

      {#if events.length > 0}
        <details>
          <summary class="muted small">Events and gaps ({events.length})</summary>
          <ul class="events">
            {#each events as event (event.sequence)}
              <li>
                <span class="numeric tiny">
                  {new Date(event.clientEpochMs).toLocaleString()}
                </span>
                <span class:warn={event.type !== 'MARK'}>{event.type}</span>
                <span class="muted">{event.label ?? ''}</span>
              </li>
            {/each}
          </ul>
        </details>
      {/if}
    </div>
  {/if}
</section>

<style>
  .page { display: grid; gap: 0.7rem; align-content: start; }
  .head { display: flex; justify-content: space-between; align-items: flex-start;
          gap: 1rem; flex-wrap: wrap; }
  .usage { text-align: right; display: grid; gap: 0.1rem; }
  .panel { background: var(--surface); border: 1px solid var(--line);
           border-radius: 8px; padding: 0.7rem 0.8rem; display: grid; gap: 0.5rem; }
  .panel.empty { text-align: center; color: var(--muted); }
  .interrupted { border-left: 3px solid var(--warn); }
  .interrupted ul { list-style: none; margin: 0; padding: 0; display: grid; gap: 0.25rem; }
  .interrupted li { display: flex; gap: 0.6rem; align-items: baseline;
                    font-size: 0.8rem; }
  .interrupted li button { margin-left: auto; }
  .danger-panel { border-left: 3px solid var(--danger); }
  table { width: 100%; border-collapse: collapse; font-size: 0.82rem; }
  th { text-align: left; font-weight: 600; font-size: 0.68rem; text-transform: uppercase;
       letter-spacing: 0.04em; color: var(--muted); padding: 0.2rem 0.4rem;
       border-bottom: 1px solid var(--line); }
  td { padding: 0.35rem 0.4rem; border-bottom: 1px solid var(--line);
       vertical-align: top; }
  tr.selected { background: var(--surface-2); }
  .right { text-align: right; }
  .actions { display: flex; gap: 0.25rem; justify-content: flex-end; }
  .sub { display: flex; align-items: baseline; gap: 0.6rem; }
  .sub .close { margin-left: auto; background: none; border: 0; color: var(--muted);
                font-size: 1.2rem; cursor: pointer; }
  .events { list-style: none; margin: 0.4rem 0 0; padding: 0; display: grid;
            gap: 0.15rem; font-size: 0.78rem; }
  .events li { display: flex; gap: 0.5rem; }
  .row { display: flex; gap: 0.4rem; }
  .muted { color: var(--muted); }
  .warn { color: var(--warn); }
  .ok { color: var(--ok); }
  .accent { color: var(--accent); }
  .bad { color: var(--danger); }
  .small { font-size: 0.75rem; }
  .tiny { font-size: 0.72rem; }
  .numeric { font-family: var(--font-mono); }
  button { background: var(--surface-2); border: 1px solid var(--line); color: var(--text);
           border-radius: 6px; padding: 0.2rem 0.5rem; cursor: pointer; font: inherit;
           font-size: 0.74rem; }
  button.link { background: none; border: 0; color: var(--accent); padding: 0;
                font-size: 0.82rem; text-align: left; }
  button.danger { color: var(--danger); border-color: var(--danger); }
  button:disabled { opacity: 0.45; cursor: default; }
  input { background: var(--surface-2); border: 1px solid var(--line); color: var(--text);
          border-radius: 6px; padding: 0.25rem 0.4rem; font: inherit; font-size: 0.8rem;
          flex: 1; }
</style>
