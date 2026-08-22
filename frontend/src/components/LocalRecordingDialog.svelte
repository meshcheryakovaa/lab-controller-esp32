<script lang="ts">
  // ===========================================================================
  //  LocalRecordingDialog — starting a recording on THIS device (M14).
  //
  //  The limitations are on the same screen as the Start button, not behind a
  //  help link.  An operator who leaves a tablet running overnight and finds a
  //  four-hour hole because the screen locked was not warned by a tooltip they
  //  never opened.  §4 lists what must be said; this says it before the choice
  //  is made, which is the only time it is useful.
  // ===========================================================================
  import { controller } from '../lib/state.svelte';
  import { recorder } from '../lib/client-recorder.svelte';
  import type { RecorderChannel } from '../lib/client-recorder';
  import {
    estimateBytes, rateLabel, type LocalRateMode,
  } from '../lib/local-history-types';
  import { formatBytes } from '../lib/format';

  let {
    dashboardKey = '',
    suggested = [] as string[],
    onclose = () => {},
    onstarted = () => {},
  }: {
    dashboardKey?: string;
    suggested?: string[];
    onclose?: () => void;
    onstarted?: () => void;
  } = $props();

  const RATES: LocalRateMode[] = ['0.1Hz', '0.2Hz', '1Hz', '5Hz', 'every'];

  const available = $derived(controller.channels.filter((c) => c.visible));

  // Seeded from what this dashboard actually shows: the operator asked to
  // record "what I am looking at", and making them re-pick it is a way to get
  // it wrong.
  let picked = $state<Set<string> | null>(null);
  // Seeded from the prop the first time it is non-empty, then owned by the
  // operator: re-seeding on every change would undo their choices.
  $effect(() => {
    if (picked === null) picked = new Set(suggested);
  });
  const selection = $derived(picked ?? new Set(suggested));
  let name = $state('');
  let operator = $state('');
  let sample = $state('');
  let rate = $state<LocalRateMode>('1Hz');
  let starting = $state(false);
  let error = $state('');
  let quota = $state<{ used: number; total: number; persisted: boolean } | null>(null);

  $effect(() => {
    if (name === '') {
      name = `${dashboardKey || 'Recording'} ${new Date().toLocaleString()}`;
    }
  });

  $effect(() => {
    void (async () => {
      const db = recorder.database;
      if (!db) return;
      const usage = await db.usage(controller.controllerId);
      quota = {
        used: usage.sessionsBytes,
        total: Math.max(0, usage.quotaBytes - usage.usedBytes),
        persisted: usage.persisted,
      };
    })();
  });

  function toggle(key: string) {
    const next = new Set(selection);
    if (next.has(key)) next.delete(key); else next.add(key);
    picked = next;
  }

  const chosen = $derived(available.filter((c) => selection.has(c.key)));
  const perHour = $derived(estimateBytes(chosen.length, rate, 3_600_000));
  const perDay = $derived(estimateBytes(chosen.length, rate, 86_400_000));
  // The estimate is only useful next to the room actually left.
  const tight = $derived(
    quota !== null && quota.total > 0 && perDay > quota.total * 0.5);

  async function start() {
    error = '';
    starting = true;
    try {
      const channels: RecorderChannel[] = chosen.map((c) => ({
        handle: c.handle,
        key: c.key,
        name: c.name,
        unit: c.unit ?? '',
        quantity: c.quantity ?? '',
        precision: typeof c.precision === 'number' ? c.precision : 3,
        calibrationId: controller.activeCalibration(c.key)?.id,
      }));
      await recorder.start({
        dashboardKey,
        name: name.trim() || 'Local recording',
        operator: operator.trim(),
        sample: sample.trim(),
        firmwareVersion: String(controller.system?.firmware ?? ''),
        configRevision: Number(controller.system?.config_revision ?? -1),
        rateMode: rate,
        channels,
      });
      // The subscription must now include these channels even if no widget on
      // screen wants them — otherwise walking to Hardware would starve the run.
      controller.subscribeToVisibleChannels();
      onstarted();
      onclose();
    } catch (e) {
      error = e instanceof Error ? e.message : String(e);
    } finally {
      starting = false;
    }
  }
</script>

<div class="scrim" role="presentation" onclick={onclose}></div>
<div class="dialog" role="dialog" aria-modal="true" aria-label="Record on this device">
  <header>
    <strong>Record on this device</strong>
    <button type="button" class="close" onclick={onclose} aria-label="Close">×</button>
  </header>

  {#if !recorder.status.available}
    <p class="bad">{recorder.status.unavailableReason}</p>
  {:else}
    <div class="grid">
      <label class="field">
        <span class="label">Name</span>
        <input type="text" bind:value={name} />
      </label>
      <label class="field">
        <span class="label">Operator</span>
        <input type="text" bind:value={operator} placeholder="who is running this" />
      </label>
      <label class="field">
        <span class="label">Sample</span>
        <input type="text" bind:value={sample} placeholder="what is being measured" />
      </label>
    </div>

    <div class="field">
      <span class="label">Channels ({chosen.length} of {available.length})</span>
      <ul class="channels">
        {#each available as channel (channel.key)}
          <li>
            <label>
              <input type="checkbox" checked={selection.has(channel.key)}
                     onchange={() => toggle(channel.key)} />
              <span class="numeric">{channel.key}</span>
              <span class="muted">{channel.name}{channel.unit ? ` (${channel.unit})` : ''}</span>
            </label>
          </li>
        {/each}
        {#if available.length === 0}
          <li class="muted small">This controller has no visible channels.</li>
        {/if}
      </ul>
    </div>

    <fieldset class="field">
      <legend class="label">Rate</legend>
      {#each RATES as option (option)}
        <label class="radio">
          <input type="radio" name="rate" value={option}
                 checked={rate === option} onchange={() => (rate = option)} />
          <span>{rateLabel(option)}</span>
          {#if option === 'every'}
            <span class="muted small">
              — every batch this browser receives, not every sensor sample
            </span>
          {/if}
        </label>
      {/each}
    </fieldset>

    <div class="estimate">
      <div><span class="muted">Estimated size</span>
        <strong>{formatBytes(perHour)}</strong> per hour ·
        <strong>{formatBytes(perDay)}</strong> per 24 h</div>
      {#if quota}
        <div class="muted small">
          Already used by local recordings: {formatBytes(quota.used)} ·
          approximately {formatBytes(quota.total)} left to this browser
          {#if quota.persisted}· storage marked persistent{/if}
        </div>
      {/if}
      {#if tight}
        <div class="warn small">
          A 24-hour run at this rate would use more than half the room left.
          Pick a slower rate, fewer channels, or export and delete an old set first.
        </div>
      {/if}
    </div>

    <details class="limits" open>
      <summary>What this does and does not promise</summary>
      <ul>
        <li>The data is written <strong>only on this device, in this browser
            profile</strong>. Another computer will not see it.</li>
        <li>Clearing site data in this browser deletes it.</li>
        <li>This tab must stay open. On a tablet, stop the screen locking and
            keep it on power.</li>
        <li>Losing Wi-Fi leaves a <strong>marked gap</strong>. Nothing is
            invented across it.</li>
        <li>For an experiment that matters, run the ESP32's own logger or SD
            card at the same time — it keeps recording with no browser at all.</li>
        <li>For a permanent archive, export the set to CSV. Browser storage is
            not a place to leave scientific data.</li>
      </ul>
    </details>

    {#if error}<p class="bad">{error}</p>{/if}

    <footer>
      <button type="button" onclick={onclose}>Cancel</button>
      <button type="button" class="primary"
              disabled={starting || chosen.length === 0}
              onclick={start}>
        {starting ? 'Starting…' : 'Start recording'}
      </button>
    </footer>
    {#if chosen.length === 0}
      <p class="muted small">Pick at least one channel — a recording of nothing
        is not a recording.</p>
    {/if}
  {/if}
</div>

<style>
  .scrim { position: fixed; inset: 0; background: rgba(4, 8, 13, 0.6); z-index: 40; }
  .dialog { position: fixed; z-index: 41; top: 50%; left: 50%;
            transform: translate(-50%, -50%); width: min(560px, calc(100vw - 2rem));
            max-height: calc(100vh - 3rem); overflow: auto;
            background: var(--surface); border: 1px solid var(--line);
            border-radius: 10px; padding: 0.9rem 1rem; display: grid; gap: 0.7rem; }
  header { display: flex; justify-content: space-between; align-items: center; }
  .close { background: none; border: 0; color: var(--muted); font-size: 1.3rem;
           line-height: 1; cursor: pointer; }
  .grid { display: grid; grid-template-columns: 1fr 1fr; gap: 0.5rem; }
  .grid .field:first-child { grid-column: 1 / -1; }
  .field { display: grid; gap: 0.25rem; }
  .label { font-size: 0.68rem; letter-spacing: 0.05em; text-transform: uppercase;
           opacity: 0.75; }
  fieldset { border: 0; padding: 0; margin: 0; display: grid; gap: 0.2rem; }
  legend { padding: 0; }
  .channels { list-style: none; margin: 0; padding: 0.3rem; display: grid;
              gap: 0.15rem; max-height: 170px; overflow: auto;
              border: 1px solid var(--line); border-radius: 6px; }
  .channels label, .radio { display: flex; align-items: baseline; gap: 0.4rem;
                            font-size: 0.8rem; cursor: pointer; }
  .estimate { display: grid; gap: 0.2rem; font-size: 0.8rem;
              border-top: 1px solid var(--line); padding-top: 0.5rem; }
  .limits { font-size: 0.76rem; }
  .limits summary { cursor: pointer; color: var(--muted); }
  .limits ul { margin: 0.4rem 0 0; padding-left: 1.1rem; display: grid; gap: 0.2rem; }
  footer { display: flex; justify-content: flex-end; gap: 0.4rem; }
  .muted { color: var(--muted); }
  .small { font-size: 0.72rem; }
  .warn { color: var(--warn); }
  .bad { color: var(--danger); font-size: 0.8rem; margin: 0; }
  .numeric { font-family: var(--font-mono); font-size: 0.76rem; }
  input[type=text] { background: var(--surface-2); border: 1px solid var(--line);
                     color: var(--text); border-radius: 6px; padding: 0.3rem 0.4rem;
                     font: inherit; font-size: 0.82rem; width: 100%; }
  button { background: var(--surface-2); border: 1px solid var(--line);
           color: var(--text); border-radius: 6px; padding: 0.32rem 0.7rem;
           cursor: pointer; font: inherit; font-size: 0.82rem; }
  button.primary { background: var(--accent); border-color: var(--accent); color: #06121f;
                   font-weight: 600; }
  button:disabled { opacity: 0.5; cursor: default; }
</style>
