<script lang="ts">
  // ===========================================================================
  //  LocalRecordingStatus — the indicator that follows the operator (M14).
  //
  //  Rendered in the application frame, not on the Dashboard: a recording that
  //  is running, or that STOPPED because the device filled up, is something
  //  every page has to keep saying.  The failure this prevents is coming back
  //  to a tablet after six hours to find the recording died in the first ten
  //  minutes and nothing on screen mentioned it (§22).
  // ===========================================================================
  import { recorder } from '../lib/client-recorder.svelte';
  import { formatBytes, formatDuration } from '../lib/format';

  let { compact = false, onopen = () => {} }: {
    compact?: boolean;
    onopen?: () => void;
  } = $props();

  const status = $derived(recorder.status);

  // Ticks so the elapsed time moves without the recorder having to publish it.
  let now = $state(Date.now());
  $effect(() => {
    if (!status.active) return;
    const timer = setInterval(() => (now = Date.now()), 1000);
    return () => clearInterval(timer);
  });

  const elapsed = $derived(
    status.startedClientEpochMs ? now - status.startedClientEpochMs : 0);

  // A recording that ended badly stays on screen until it is acknowledged.
  const stoppedBadly = $derived(
    !status.active && (status.state === 'FULL' || status.state === 'ERROR'));

  let marking = $state(false);
  let markLabel = $state('');

  async function addMark() {
    const label = markLabel.trim();
    if (!label) return;
    await recorder.mark(label);
    markLabel = '';
    marking = false;
  }

  let dismissed = $state(false);
</script>

{#if status.active}
  <div class="bar" class:compact>
    <span class="dot" aria-hidden="true"></span>
    <span class="what">
      <strong>Recording on this device</strong>
      {#if !compact}<span class="muted">· {status.name}</span>{/if}
    </span>
    <span class="numbers numeric">
      {formatDuration(elapsed)} · {status.channels} ch ·
      {(status.rows + status.pendingRows).toLocaleString()} rows
      {#if status.pendingRows > 0}
        <span class="muted">({status.pendingRows} not yet written)</span>
      {/if}
      · {formatBytes(status.bytes)}
      {#if status.gaps > 0}
        <span class="warn">· gaps: {status.gaps}</span>
      {:else}
        · gaps: 0
      {/if}
      {#if status.droppedRows > 0}
        <span class="warn">· dropped: {status.droppedRows}</span>
      {/if}
    </span>
    {#if !status.connected}
      <span class="warn">link down — this period will be a gap</span>
    {/if}
    <span class="actions">
      {#if marking}
        <input type="text" bind:value={markLabel} placeholder="what happened"
               onkeydown={(e) => { if (e.key === 'Enter') void addMark(); }} />
        <button type="button" onclick={() => void addMark()}>Save</button>
        <button type="button" onclick={() => (marking = false)}>Cancel</button>
      {:else}
        <button type="button" onclick={() => (marking = true)}>Mark event</button>
        <button type="button" onclick={onopen}>Local data</button>
        <button type="button" class="danger"
                onclick={() => void recorder.stop()}>Stop</button>
      {/if}
    </span>
  </div>
{:else if stoppedBadly && !dismissed}
  <div class="bar bad">
    <span class="what">
      <strong>
        {status.state === 'FULL'
          ? 'Local recording stopped: this device is out of room'
          : 'Local recording stopped after an error'}
      </strong>
      <span class="muted">
        {status.stopReason ?? status.lastError ?? ''}
      </span>
    </span>
    <span class="muted small">
      The controller kept measuring, and its own logger was not affected.
    </span>
    <span class="actions">
      <button type="button" onclick={onopen}>Local data</button>
      <button type="button" onclick={() => (dismissed = true)}>Dismiss</button>
    </span>
  </div>
{:else if status.ownedElsewhere}
  <div class="bar quiet">
    <span class="what muted">
      Another tab of this browser is recording this controller. Only one may
      write, so this tab is watching.
    </span>
  </div>
{/if}

<style>
  .bar { display: flex; align-items: center; gap: 0.55rem; flex-wrap: wrap;
         background: var(--surface-2); border: 1px solid var(--line);
         border-left: 3px solid var(--ok); border-radius: 7px;
         padding: 0.35rem 0.6rem; font-size: 0.78rem; }
  .bar.bad { border-left-color: var(--danger); }
  .bar.quiet { border-left-color: var(--line); }
  .dot { width: 8px; height: 8px; border-radius: 50%; background: var(--ok);
         box-shadow: 0 0 0 0 rgba(63, 185, 80, 0.6); animation: pulse 2s infinite; }
  @keyframes pulse {
    70% { box-shadow: 0 0 0 6px rgba(63, 185, 80, 0); }
    100% { box-shadow: 0 0 0 0 rgba(63, 185, 80, 0); }
  }
  @media (prefers-reduced-motion: reduce) { .dot { animation: none; } }
  .numbers { font-size: 0.74rem; color: var(--muted); }
  .actions { margin-left: auto; display: flex; gap: 0.3rem; align-items: center; }
  .muted { color: var(--muted); }
  .warn { color: var(--warn); }
  .small { font-size: 0.72rem; }
  .numeric { font-family: var(--font-mono); }
  button { background: var(--surface); border: 1px solid var(--line); color: var(--text);
           border-radius: 6px; padding: 0.2rem 0.5rem; cursor: pointer; font: inherit;
           font-size: 0.74rem; }
  button.danger { color: var(--danger); border-color: var(--danger); }
  input { background: var(--surface); border: 1px solid var(--line); color: var(--text);
          border-radius: 6px; padding: 0.2rem 0.4rem; font: inherit; font-size: 0.74rem; }
</style>
