<script lang="ts">
  import { formatValue, qualityClass, qualityLabel } from '../lib/format';
  import { controller } from '../lib/state.svelte';
  import type { Channel } from '../lib/types';

  let { channel }: { channel: Channel } = $props();

  const value = $derived(controller.values[channel.handle]);
  // qualityOf, not the raw map: with the socket down the newest number we hold
  // is a snapshot of unknown age, and it must not be painted as fresh.
  const quality = $derived(controller.qualityOf(channel.handle));
  // Which device produced this — more useful under a reading than repeating
  // the physical quantity, which the unit already says.
  const source = $derived(
    channel.source === 0
      ? 'virtual'
      : (controller.deviceByHandle(channel.source)?.key ?? channel.key),
  );
</script>

<div class="tile">
  <div class="name" title={channel.key}>{channel.name}</div>
  <div class="reading">
    <span class="value numeric {qualityClass(quality)}">
      {formatValue(value, channel.precision, quality)}
    </span>
    <span class="unit">{channel.unit}</span>
  </div>
  <!-- A degraded reading is labelled, never quietly shown as if it were fine. -->
  <div class="footer">
    {#if qualityLabel(quality)}
      <span class="flag {qualityClass(quality)}">{qualityLabel(quality)}</span>
    {:else}
      <span class="muted">{source}</span>
    {/if}
  </div>
</div>

<style>
  .tile {
    background: var(--surface); border: 1px solid var(--line); border-radius: 8px;
    padding: 0.6rem 0.75rem; display: grid; gap: 0.1rem; min-width: 0;
  }
  .name { font-size: 0.72rem; color: var(--muted); overflow: hidden;
          text-overflow: ellipsis; white-space: nowrap; }
  .reading { display: flex; align-items: baseline; gap: 0.3rem; }
  .value { font-size: 1.6rem; font-weight: 500; letter-spacing: -0.01em; }
  .unit { font-size: 0.8rem; color: var(--muted); }
  .footer { font-size: 0.68rem; }
  .muted { color: var(--muted); }
  .flag { text-transform: uppercase; letter-spacing: 0.05em; font-weight: 600; }
  :global(.q-good)    { color: var(--text); }
  :global(.q-stale)   { color: var(--warn); }
  :global(.q-range)   { color: var(--warn); }
  :global(.q-fault)   { color: var(--danger); }
  :global(.q-unknown) { color: var(--muted); }
</style>
