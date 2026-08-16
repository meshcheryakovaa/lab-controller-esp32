<script lang="ts">
  // A chart of several channels.  The plotting itself is LineChart (uPlot); this
  // wrapper turns the stored series list into handles and says plainly when one
  // of them no longer exists, instead of quietly plotting one line fewer.
  import LineChart from '../LineChart.svelte';
  import { controller } from '../../lib/state.svelte';
  import type { WidgetConfig } from '../../lib/widgets';

  let { config, height = 200 }: { config: WidgetConfig; height?: number } = $props();

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
    <LineChart {handles}
               windowSeconds={typeof config.window_s === 'number' ? config.window_s : 0}
               height={Math.max(120, height - (config.title ? 22 : 0) - (missing.length ? 20 : 0))} />
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
</style>
