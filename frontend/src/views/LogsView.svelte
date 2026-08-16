<script lang="ts">
  // ===========================================================================
  //  LogsView — the datasets this rig has produced.
  //
  //  Two things this page refuses to be coy about:
  //    * a TRUNCATED dataset is marked wherever it appears, and the reason is
  //      next to it.  A file that stops early and looks complete is the one
  //      that gets published (ADR-0019);
  //    * how much room is left, in the same units as the estimate the firmware
  //      used to accept the session.  "Disk almost full" is not an actionable
  //      sentence; "212 KB writable, 64 KB reserved" is.
  //
  //  Downloads are hrefs, not fetches: a dataset belongs in the browser's
  //  download stream, not in a JavaScript string on its way to a Blob.
  // ===========================================================================
  import { onMount } from 'svelte';
  import { api, ApiRequestError } from '../lib/api';
  import { errorSentence } from '../lib/format';
  import { controller, describe } from '../lib/state.svelte';
  import type { LogEntry } from '../lib/types';

  let entries = $state<LogEntry[]>([]);
  let limits = $state({ sessions: 24, channels: 16, rate_hz: 50 });
  let busy = $state(false);
  let error = $state('');

  // Manual recording — for the operator who is not running a scenario.
  let selected = $state<string[]>([]);
  let rate = $state(1);
  let name = $state('');
  let operatorName = $state('');
  let sampleName = $state('');

  const logging = $derived(controller.logging);
  // `logged` is the descriptor's own answer to "does this belong in a
  // dataset".  The experiment state channels say no: they exist so rules and
  // dashboards can read them, not to become a column of enum values.
  const inputs = $derived(
    controller.channels.filter((c) => c.direction === 'input' && c.logged !== false),
  );

  onMount(() => {
    void reload();
    const timer = setInterval(() => void reload(), 2000);
    return () => clearInterval(timer);
  });

  async function reload() {
    try {
      const response = await api.logs();
      entries = response.logs;
      limits = response.limits;
      controller.logging = response.recording;
    } catch (e) {
      error = describe(e);
    }
  }

  async function act(action: () => Promise<unknown>) {
    busy = true;
    error = '';
    try {
      await action();
      await reload();
    } catch (e) {
      error = e instanceof ApiRequestError ? errorSentence(e.error) : describe(e);
    } finally {
      busy = false;
    }
  }

  function start() {
    void act(() => api.startLog({
      name: name || 'manual recording',
      operator: operatorName,
      sample: sampleName,
      rate_hz: rate,
      channels: selected,
    }));
  }

  function toggle(key: string) {
    selected = selected.includes(key)
      ? selected.filter((k) => k !== key)
      : [...selected, key];
  }

  function size(bytes: number): string {
    if (bytes < 1024) return `${bytes} B`;
    if (bytes < 1024 * 1024) return `${(bytes / 1024).toFixed(1)} KB`;
    return `${(bytes / (1024 * 1024)).toFixed(2)} MB`;
  }

  function when(epochMs?: number): string {
    if (!epochMs) return 'clock not set';
    return new Date(epochMs).toLocaleString();
  }

  // What the rig can still record, in the same units the firmware refuses in.
  const writable = $derived(logging?.writable_bytes ?? 0);
  const reserve = $derived(logging?.reserve_bytes ?? 0);
  const low = $derived(writable > 0 && writable < 128 * 1024);
</script>

<div class="page">
  {#if error}<div class="banner bad-banner">{error}</div>{/if}

  <section class="panel">
    <div class="panel-head">
      <h2>Recording</h2>
      {#if logging?.recording}
        <span class="pill ok">RECORDING</span>
      {:else if logging?.last_truncated}
        <span class="pill bad">LAST ONE TRUNCATED</span>
      {/if}
      <span class="muted small">
        {size(writable)} writable · {size(reserve)} reserved for the instrument
        itself — samples never cross that line.
      </span>
    </div>

    {#if low && !logging?.recording}
      <p class="small warn">
        There is not much room left. A session that will not fit is refused when
        it starts, not discovered eight hours in — but deleting a dataset you
        have already taken away is the way to make room.
      </p>
    {/if}

    {#if logging?.recording}
      <div class="running">
        <div>
          <span class="label">dataset</span>
          <strong class="numeric">{logging.id}</strong>
          <span class="muted small">{logging.name}</span>
        </div>
        <div>
          <span class="label">rows</span>
          <span class="numeric">{logging.rows}</span>
          {#if logging.dropped_rows > 0}
            <span class="warn small">{logging.dropped_rows} dropped</span>
          {/if}
        </div>
        <div>
          <span class="label">written</span>
          <span class="numeric">{size(logging.bytes)}</span>
          <span class="muted small">{logging.rate_hz} Hz · {logging.channels} ch</span>
        </div>
        <button type="button" class="danger" disabled={busy}
                onclick={() => act(() => api.stopLog())}>Stop recording</button>
      </div>
    {:else}
      <div class="start">
        <label>name<input bind:value={name} placeholder="what this is" /></label>
        <label>operator<input bind:value={operatorName} /></label>
        <label>sample<input bind:value={sampleName} /></label>
        <label>rate (Hz)
          <input type="number" min="0.1" max={limits.rate_hz} step="0.1" bind:value={rate} />
        </label>
        <button type="button" class="primary"
                disabled={busy || selected.length === 0 || controller.runningExperiment}
                onclick={start}>Record</button>
      </div>

      <div class="channels">
        {#each inputs as channel (channel.key)}
          <label class="check">
            <input type="checkbox" checked={selected.includes(channel.key)}
                   disabled={!selected.includes(channel.key) &&
                             selected.length >= limits.channels}
                   onchange={() => toggle(channel.key)} />
            <span class="numeric">{channel.key}</span>
            <span class="muted small">{channel.unit}</span>
          </label>
        {/each}
      </div>

      {#if controller.runningExperiment}
        <p class="small muted">
          A run is in progress; it records what its scenario says to record.
        </p>
      {/if}
      {#if logging?.last_truncated}
        <p class="small bad">
          The last dataset stopped early: {logging.last_error || logging.last_stop}.
          It is kept, and it says so.
        </p>
      {/if}
    {/if}
  </section>

  <section class="panel">
    <div class="panel-head">
      <h2>Datasets</h2>
      <span class="muted small">
        {entries.length} of {limits.sessions} · nothing here is ever deleted to
        make room; that decision is yours.
      </span>
    </div>

    {#if entries.length === 0}
      <p class="muted small">No datasets yet.</p>
    {:else}
      <table>
        <thead>
          <tr>
            <th>Id</th><th>What</th><th>Started</th>
            <th class="right">Rows</th><th class="right">Size</th>
            <th>State</th><th>Configuration</th><th></th>
          </tr>
        </thead>
        <tbody>
          {#each entries as entry (entry.id)}
            <tr class:truncated={entry.truncated}>
              <td class="numeric small">{entry.id}</td>
              <td>
                {entry.name}
                {#if entry.experiment}
                  <div class="muted small">
                    {entry.experiment} · {entry.operator || '—'}
                    {#if entry.sample}· {entry.sample}{/if}
                  </div>
                {/if}
              </td>
              <td class="small">{when(entry.started_epoch_ms)}</td>
              <td class="right numeric small">
                {entry.rows}
                {#if entry.dropped > 0}
                  <div class="warn small">{entry.dropped} dropped</div>
                {/if}
              </td>
              <td class="right numeric small">{size(entry.bytes)}</td>
              <td>
                {#if entry.state === 'TRUNCATED'}
                  <span class="pill bad">TRUNCATED</span>
                  <div class="muted small">{entry.reason}</div>
                {:else if entry.state === 'RECORDING'}
                  <span class="pill ok">RECORDING</span>
                {:else}
                  <span class="pill muted">complete</span>
                {/if}
              </td>
              <td class="small muted">
                {#if entry.config_fingerprint !== undefined}
                  <span class="numeric">
                    {entry.config_fingerprint.toString(16).padStart(8, '0')}
                  </span>
                {/if}
                <div>{entry.firmware ?? ''} · {entry.channels} ch @ {entry.rate_hz} Hz</div>
              </td>
              <td class="row-actions">
                <a class="button" href={api.logDownloadUrl(entry.id)}
                   download={`${entry.id}.csv`}>CSV</a>
                <button type="button" disabled={busy || entry.state === 'RECORDING'}
                        onclick={() => {
                          if (confirm(`Delete ${entry.id}? Take the CSV first if you want it.`)) {
                            void act(() => api.deleteLog(entry.id));
                          }
                        }}>Delete</button>
              </td>
            </tr>
          {/each}
        </tbody>
      </table>
    {/if}
  </section>
</div>

<style>
  .page { display: grid; gap: 1rem; }
  .panel { background: var(--surface); border: 1px solid var(--line);
           border-radius: 8px; padding: 0.8rem 0.9rem; display: grid; gap: 0.6rem; }
  .panel-head { display: flex; align-items: baseline; gap: 0.6rem; flex-wrap: wrap; }
  .panel-head > span.muted { flex: 1 1 16rem; }
  h2 { margin: 0; font-size: 0.8rem; text-transform: uppercase;
       letter-spacing: 0.07em; color: var(--muted); font-weight: 600; }
  table { width: 100%; border-collapse: collapse; font-size: 0.85rem; }
  th { text-align: left; font-weight: 500; font-size: 0.68rem; text-transform: uppercase;
       letter-spacing: 0.05em; color: var(--muted); padding: 0 0.6rem 0.35rem 0; }
  th.right, td.right { text-align: right; }
  td { padding: 0.35rem 0.4rem 0.35rem 0; border-top: 1px solid var(--line);
       vertical-align: top; }
  tr.truncated { background: color-mix(in srgb, var(--danger) 10%, transparent); }
  input { background: var(--surface-2); border: 1px solid var(--line);
                  color: var(--text); border-radius: 5px; padding: 0.18rem 0.35rem;
                  font: inherit; font-size: 0.8rem; }
  button, .button { background: var(--surface-2); border: 1px solid var(--line);
           color: var(--text); border-radius: 6px; padding: 0.2rem 0.55rem;
           cursor: pointer; font: inherit; font-size: 0.75rem;
           text-decoration: none; display: inline-block; }
  button:disabled { opacity: 0.5; cursor: default; }
  button.primary { background: var(--accent); border-color: var(--accent);
                   color: #05121f; font-weight: 600; }
  button.danger { color: var(--danger); border-color: var(--danger); }
  .row-actions { white-space: nowrap; text-align: right; }
  .row-actions .button, .row-actions button { margin-left: 0.25rem; }
  .start, .running { display: flex; gap: 0.6rem; align-items: center; flex-wrap: wrap; }
  .running > div { display: grid; }
  .start label { display: grid; gap: 0.15rem; font-size: 0.7rem; color: var(--muted); }
  .label { font-size: 0.62rem; text-transform: uppercase; letter-spacing: 0.06em;
           color: var(--muted); }
  .channels { display: flex; flex-wrap: wrap; gap: 0.5rem; }
  .check { display: inline-flex; align-items: center; gap: 0.3rem;
           font-size: 0.78rem; }
  .banner { border-radius: 8px; padding: 0.5rem 0.7rem; font-size: 0.8rem; }
  .bad-banner { background: color-mix(in srgb, var(--danger) 14%, transparent);
                border: 1px solid var(--danger); }
  .pill { font-size: 0.6rem; text-transform: uppercase; letter-spacing: 0.06em;
          font-weight: 700; border: 1px solid currentColor; border-radius: 3px;
          padding: 0 0.25rem; }
  .pill.ok { color: var(--ok); }
  .pill.bad { color: var(--danger); }
  .pill.muted { color: var(--muted); }
  .muted { color: var(--muted); }
  .small { font-size: 0.72rem; }
  .warn { color: var(--warn); }
  .bad { color: var(--danger); }
  .numeric { font-family: var(--font-mono); }
  p { margin: 0; }
</style>
