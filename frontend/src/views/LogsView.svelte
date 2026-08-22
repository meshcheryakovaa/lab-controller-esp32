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
  import { offload } from '../lib/log-offload/offload.svelte';
  import { mergedCsv, sessionZip } from '../lib/log-offload/SegmentExport';
  import type { StoredEspSession } from '../lib/log-offload/SegmentArchive';
  import { download, toByteStream } from '../lib/local-history-client';
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

  // M15: where the dataset lives while it is written.  `single` is the default
  // and the old behaviour; `continuous_offload` turns the controller's flash
  // into a transfer buffer and this tab into the archive.
  let storageMode = $state<'single' | 'continuous_offload'>('single');
  let roomWarning = $state('');

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
    void act(async () => {
      if (storageMode === 'continuous_offload') {
        // A device that cannot hold two parts cannot keep up with a rotation,
        // and the controller's queue would start filling immediately (§17).
        if (!offload.available) throw new Error(offload.unavailableReason);
        if (!(await offload.hasRoom(segmentBytes))) {
          throw new Error('this device has too little room left to collect '
                        + 'segments; export and delete a local set first');
        }
      }
      const started = await api.startLog({
        name: name || 'manual recording',
        operator: operatorName,
        sample: sampleName,
        rate_hz: rate,
        channels: selected,
        storage_mode: storageMode,
        ...(storageMode === 'continuous_offload'
          ? { segment_bytes: segmentBytes, collector_id: offload.id }
          : {}),
      });
      if (storageMode === 'continuous_offload' && started.id) {
        await offload.attach(started.id, {
          name: name || 'manual recording',
          operator: operatorName,
          sample: sampleName,
          rateHz: rate,
          channels: selected.length,
          startedEpochMs: Date.now(),
        });
      }
    });
  }

  /** What the firmware uses when asked for nothing in particular. */
  const DEFAULT_SEGMENT_BYTES = 100 * 1024;
  const MIN_SEGMENT_BYTES = 32 * 1024;


  const collector = $derived(offload.status);

  /**
   * What the handover is doing, in words an operator can act on.
   *
   * The state machine's WAITING_FOR_DEVICE covers two very different
   * situations — "nothing to collect right now" and "the controller stopped
   * answering" — and showing the raw name made the ordinary one look like a
   * fault.
   */
  const handoverPhrase = $derived.by(() => {
    switch (collector.state) {
      case 'WAITING_FOR_DEVICE':
        return collector.attempts > 0 ? 'no answer from the controller'
                                      : 'up to date';
      case 'RECEIVING': return 'receiving a part';
      case 'VERIFYING': return 'checking a part';
      case 'SAVING': return 'saving to this device';
      case 'ACKNOWLEDGING': return 'confirming to the controller';
      case 'LOCAL_STORAGE_FULL': return 'this device is full';
      case 'COMPLETE': return 'everything has been handed over';
      case 'ERROR': return 'stopped after an error';
      default: return collector.state.replace(/_/g, ' ').toLowerCase();
    }
  });
  const collecting = $derived(
    collector.sessionId !== '' && collector.state !== 'IDLE');
  // How long the controller could keep recording with nobody collecting.
  const queueHeadroom = $derived(
    collector.segmentBytes > 0
      ? Math.floor(collector.writableBytes / collector.segmentBytes)
      : 0);

  async function exportMerged(session: StoredEspSession) {
    await act(async () => {
      const report = { segments: 0, rows: 0, missing: [] as number[], complete: false };
      await download(toByteStream(mergedCsv(offload.store!, session, report)),
                     `${session.sessionId}.csv`, 'text/csv');
    });
  }

  async function exportZip(session: StoredEspSession) {
    await act(async () => {
      await download(await sessionZip(offload.store!, session),
                     `${session.sessionId}.zip`, 'application/zip');
    });
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

  /**
   * How big one part should be on THIS controller right now.
   *
   * Continuous mode needs room for the part being written plus one waiting to
   * be collected, and on a filesystem that already holds a configuration, a few
   * dashboards and a scenario, 200 KB free is not a given.  Refusing outright
   * would be correct and useless; sizing the parts to the room that exists is
   * correct and usable.  The floor is the firmware's own minimum — below that
   * the rotation costs more in flash writes than it saves.
   */
  const segmentBytes = $derived.by(() => {
    const half = Math.floor(writable / 2 / 4096) * 4096;
    if (half >= DEFAULT_SEGMENT_BYTES) return DEFAULT_SEGMENT_BYTES;
    return Math.max(MIN_SEGMENT_BYTES, half);
  });
  const continuousFits = $derived(writable >= MIN_SEGMENT_BYTES * 2);

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

      {#if collecting}
        <div class="offload" class:bad-banner={collector.state === 'ERROR'
                                            || collector.state === 'LOCAL_STORAGE_FULL'}>
          <div>
            <span class="label">handover</span>
            <strong>{handoverPhrase}</strong>
            {#if offload.ownedElsewhere}
              <span class="warn small">— another tab is collecting this session</span>
            {/if}
          </div>
          <div>
            <span class="label">current part</span>
            <span class="numeric">{collector.activeSegment}</span>
            <span class="muted small">{size(collector.activeBytes)} / {size(collector.segmentBytes)}</span>
          </div>
          <div>
            <span class="label">on this device</span>
            <span class="numeric">{collector.collected}</span>
            <span class="muted small">parts · {size(collector.collectedBytes)}</span>
          </div>
          <div>
            <span class="label">waiting on the controller</span>
            <span class="numeric" class:warn={collector.pending > 1}>{collector.pending}</span>
            <span class="muted small">{size(collector.pendingBytes)}</span>
          </div>
          {#if collector.state === 'WAITING_FOR_DEVICE' && collector.attempts > 0}
            <p class="small warn">
              No answer from the controller. It keeps recording; room for about
              {queueHeadroom} more part{queueHeadroom === 1 ? '' : 's'} before the
              log has to stop. Nothing queued is deleted.
            </p>
          {:else if collector.state === 'LOCAL_STORAGE_FULL'}
            <p class="small bad">
              This device is full, so nothing is being acknowledged and the
              controller still holds every part. Export and delete a local set.
            </p>
          {:else if collector.state === 'ERROR'}
            <p class="small bad">{collector.lastError}</p>
          {/if}
        </div>
      {/if}
    {:else}
      <div class="start">
        <label>name<input bind:value={name} placeholder="what this is" /></label>
        <label>operator<input bind:value={operatorName} /></label>
        <label>sample<input bind:value={sampleName} /></label>
        <label>rate (Hz)
          <input type="number" min="0.1" max={limits.rate_hz} step="0.1" bind:value={rate} />
        </label>
        <fieldset class="mode">
          <legend class="label">storage</legend>
          <label class="radio">
            <input type="radio" name="storage" value="single"
                   checked={storageMode === 'single'}
                   onchange={() => (storageMode = 'single')} />
            <span>One CSV on the controller</span>
            <span class="muted small">— needs no browser; you download it later</span>
          </label>
          <label class="radio">
            <input type="radio" name="storage" value="continuous_offload"
                   checked={storageMode === 'continuous_offload'}
                   disabled={!offload.available || !continuousFits}
                   onchange={() => (storageMode = 'continuous_offload')} />
            <span>Continuous, handed to this device</span>
            <span class="muted small">
              — {size(segmentBytes)} parts, verified and then erased from the
              controller
            </span>
          </label>
          {#if !continuousFits}
            <p class="small muted">
              There is too little room on the controller for even the smallest
              parts ({size(MIN_SEGMENT_BYTES)} each, two at a time). Delete a
              dataset first.
            </p>
          {/if}
          {#if storageMode === 'continuous_offload'}
            <p class="small warn">
              This tab must stay open. The controller keeps recording if it
              closes, but only while its queue has room — and nothing already
              queued is ever deleted to make more.
            </p>
          {:else if !offload.available}
            <p class="small muted">{offload.unavailableReason}</p>
          {/if}
          {#if roomWarning}<p class="small warn">{roomWarning}</p>{/if}
        </fieldset>
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

  {#if offload.sessions.length > 0}
    <section class="panel">
      <div class="panel-head">
        <h2>Collected on this device</h2>
        <span class="muted small">
          Parts the controller handed over and then erased. This browser is the
          only place they exist — export anything worth keeping.
        </span>
      </div>
      <table>
        <thead>
          <tr>
            <th>Session</th><th>Started</th><th class="right">Parts</th>
            <th class="right">Size</th><th class="right">Rows</th>
            <th>State</th><th></th>
          </tr>
        </thead>
        <tbody>
          {#each offload.sessions as session (session.key)}
            <tr>
              <td>
                <strong>{session.name}</strong>
                <div class="muted numeric small">{session.sessionId}</div>
              </td>
              <td class="numeric small">{when(session.startedEpochMs)}</td>
              <td class="right numeric">
                {session.segmentsCollected}
                {#if session.contiguousThrough < session.segmentsCollected}
                  <span class="warn small" title="a part is missing in the middle">
                    gap
                  </span>
                {/if}
              </td>
              <td class="right numeric">{size(session.bytesCollected)}</td>
              <td class="right numeric">{session.rows}</td>
              <td class="small">{session.state}</td>
              <td class="row-actions">
                <button type="button" disabled={busy}
                        onclick={() => void exportMerged(session)}>CSV</button>
                <button type="button" disabled={busy}
                        onclick={() => void exportZip(session)}>ZIP</button>
                <button type="button" class="danger" disabled={busy}
                        onclick={() => act(() => offload.deleteSession(session.sessionId))}>
                  Delete local
                </button>
              </td>
            </tr>
          {/each}
        </tbody>
      </table>
      <p class="small muted">
        The ZIP keeps every part exactly as the controller wrote it, checksums
        and all; the merged CSV is a convenience rebuilt from them, and says so
        in its footer when a part is missing.
      </p>
    </section>
  {/if}

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
  .mode { border: 1px solid var(--line); border-radius: 6px; padding: 0.4rem 0.6rem;
          margin: 0; display: grid; gap: 0.2rem; grid-column: 1 / -1; }
  .mode legend { padding: 0 0.3rem; }
  .radio { display: flex; align-items: baseline; gap: 0.4rem; cursor: pointer;
           font-size: 0.82rem; }
  /* The page styles `input` as a full-width field; a radio is not one. */
  .mode input[type=radio] { width: auto; flex: none; margin: 0; }
  .offload { display: grid; grid-template-columns: repeat(auto-fit, minmax(150px, 1fr));
             gap: 0.5rem; align-items: baseline; border-top: 1px solid var(--line);
             margin-top: 0.5rem; padding-top: 0.5rem; }
  .offload p { grid-column: 1 / -1; margin: 0; }
  .row-actions { display: flex; gap: 0.25rem; justify-content: flex-end; }

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
