<script lang="ts">
  // A chart of several channels.  The plotting itself is LineChart (uPlot); this
  // wrapper turns the stored series list into handles and says plainly when one
  // of them no longer exists, instead of quietly plotting one line fewer.
  //
  // The three range buttons are local to the tile and cost nothing: no request,
  // no change to the sampling rate, no write to flash.  Looking at the last five
  // minutes is a way of looking, not a change to the instrument — the stored
  // default belongs to the editor.
  import LineChart from '../LineChart.svelte';
  import { controller } from '../../lib/state.svelte';
  import { rangeFromWindowSeconds, type ChartRange } from '../../lib/chart-history';
  import type { WidgetConfig } from '../../lib/widgets';

  let { config, height = 200 }: { config: WidgetConfig; height?: number } = $props();

  const RANGES: { id: ChartRange; label: string; title: string }[] = [
    { id: 'all', label: 'All', title: 'All time — the whole browser session' },
    { id: '5m', label: '5 min', title: 'Last 5 minutes' },
    { id: '10m', label: '10 min', title: 'Last 10 minutes' },
  ];

  // Height of the button row, kept in step with the CSS below so the chart is
  // not drawn taller than the tile it lives in.
  const RANGE_BAR_PX = 26;

  const resolved = $derived(
    (config.series ?? []).map((entry) => ({
      key: entry.channel,
      handle: controller.channelByKey(entry.channel)?.handle,
    })),
  );
  const handles = $derived(
    resolved.filter((s) => s.handle !== undefined).map((s) => s.handle!),
  );
  const missing = $derived(resolved.filter((s) => s.handle === undefined).map((s) => s.key));

  // The stored window decides where the buttons START; from then on the choice
  // is the operator's and is not written back.  Held as an override rather than
  // as the value itself, so a tile nobody has touched still follows the setting
  // being edited next to it.
  let override = $state<ChartRange | null>(null);
  const selectedRange = $derived(override ?? rangeFromWindowSeconds(config.window_s));
</script>

<div class="chart-widget">
  {#if config.title}<div class="title">{config.title}</div>{/if}
  {#if handles.length === 0}
    <p class="muted">
      {#if missing.length > 0}
        Every channel on this chart is gone: {missing.join(', ')}
      {:else}
        No channels selected. Open the editor and add one.
      {/if}
    </p>
  {:else}
    <div class="ranges" role="group" aria-label="Time range">
      {#each RANGES as option (option.id)}
        <button type="button"
                class:active={selectedRange === option.id}
                aria-pressed={selectedRange === option.id}
                title={option.title}
                onclick={() => (override = option.id)}>{option.label}</button>
      {/each}
    </div>
    <LineChart {handles}
               range={selectedRange}
               height={Math.max(120, height - (config.title ? 22 : 0) - RANGE_BAR_PX
                                       - (missing.length ? 20 : 0))} />
    {#if missing.length > 0}
      <p class="warn">Not plotted — no such channel: {missing.join(', ')}</p>
    {/if}
  {/if}
</div>

<style>
  .chart-widget { height: 100%; display: grid; align-content: start; gap: 0.2rem;
                  min-height: 0; }
  .title { font-size: 0.72rem; color: var(--muted); }
  .muted { color: var(--muted); font-size: 0.78rem; margin: 0; align-self: center;
           text-align: center; }
  .warn { color: var(--warn); font-size: 0.7rem; margin: 0; }
  .ranges { display: flex; gap: 0.2rem; height: 22px; }
  .ranges button { background: var(--surface-2); border: 1px solid var(--line);
                   color: var(--muted); border-radius: 5px; padding: 0 0.45rem;
                   font: inherit; font-size: 0.68rem; line-height: 20px; cursor: pointer; }
  .ranges button:hover { color: var(--text); }
  .ranges button.active { background: var(--accent); border-color: var(--accent);
                          color: #06121f; font-weight: 600; }
  .ranges button:focus-visible { outline: 2px solid var(--accent); outline-offset: 1px; }
</style>
