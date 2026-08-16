<script lang="ts">
  // ===========================================================================
  //  LineChart — uPlot wrapper.
  //
  //  History lives in the browser (see state.svelte.ts): the ESP32 has neither
  //  the RAM to keep it nor any reason to re-send it every time a chart mounts.
  //
  //  Redraw is throttled to 4 Hz regardless of how fast frames arrive.  The eye
  //  cannot use more, and on a tablet the difference between 4 and 20 redraws
  //  per second is the difference between smooth and warm.
  // ===========================================================================
  import { onMount } from 'svelte';
  import uPlot from 'uplot';
  import 'uplot/dist/uPlot.min.css';
  import { controller } from '../lib/state.svelte';

  let {
    handles = [],
    height = 260,
    // Seconds of history to show.  0 means "everything the browser is holding".
    // A widget that offers a window setting and then ignores it is worse than
    // one that never offered it.
    windowSeconds = 0,
  }: { handles?: number[]; height?: number; windowSeconds?: number } = $props();

  // Three Y axes is the documented readability limit; beyond that the reader
  // cannot tell which axis a line belongs to.
  const MAX_AXES = 3;

  let container: HTMLDivElement;
  let chart: uPlot | null = null;
  let width = $state(600);

  /**
   * The handles this chart can actually draw, in order.  A channel whose unit
   * would need a fourth axis is left out here and named under the chart —
   * previously it was still added as a series with no axis of its own, so it
   * appeared as a line the reader had no way to scale.
   */
  function plan() {
    const units: string[] = [];
    const drawn: number[] = [];
    const skipped: number[] = [];
    for (const handle of handles) {
      const unit = controller.channelByHandle(handle)?.unit ?? '';
      if (!units.includes(unit)) {
        if (units.length >= MAX_AXES) {
          skipped.push(handle);
          continue;
        }
        units.push(unit);
      }
      drawn.push(handle);
    }
    return { units, drawn, skipped };
  }

  const layout = $derived(plan());

  function buildOptions(): uPlot.Options {
    const { units, drawn } = layout;
    const channels = drawn.map((h) => controller.channelByHandle(h));

    return {
      width,
      height,
      cursor: { drag: { x: true, y: false } },
      legend: { live: true },
      scales: { x: { time: false } },
      axes: [
        { stroke: '#7d8b9a', grid: { stroke: '#1b242f' }, ticks: { stroke: '#24303d' },
          values: (_u, ticks) => ticks.map((t) => `${(t / 1000).toFixed(0)}s`) },
        ...units.map((unit, index) => ({
          stroke: '#7d8b9a',
          grid: { stroke: index === 0 ? '#1b242f' : 'transparent' },
          side: (index === 0 ? 3 : 1) as 1 | 3,
          scale: unit || 'y',
          label: unit,
        })),
      ],
      series: [
        {},
        ...channels.map((channel) => ({
          label: channel ? `${channel.name} (${channel.unit})` : '—',
          stroke: channel?.color ?? '#4c9aff',
          width: 1.5,
          scale: channel?.unit || 'y',
        })),
      ],
    };
  }

  function collect(): uPlot.AlignedData {
    // Channels update independently, so one timebase is chosen and the others
    // are sampled onto it — good enough for a live view; exact reconstruction
    // is the CSV export's job, not the dashboard's.
    //
    // The base is the channel with the MOST history, not the first one: a
    // sensor that has produced nothing yet (a load cell with no cell attached,
    // say) used to blank the whole chart just by being first in the selection.
    const drawn = layout.drawn;
    let x: number[] = [];
    for (const handle of drawn) {
      const time = controller.historyFor(handle).time;
      if (time.length > x.length) x = time;
    }
    x = x.slice();
    if (windowSeconds > 0 && x.length > 0) {
      const newest = x[x.length - 1]!;
      const oldest = newest - windowSeconds * 1000;
      const from = x.findIndex((t) => t >= oldest);
      if (from > 0) x = x.slice(from);
    }

    const series = drawn.map((handle) => {
      const history = controller.historyFor(handle);
      return x.map((t) => {
        const index = nearest(history.time, t);
        return index >= 0 ? history.value[index]! : null;
      });
    });
    return [x, ...series] as uPlot.AlignedData;
  }

  function nearest(times: number[], t: number): number {
    if (times.length === 0) return -1;
    let low = 0;
    let high = times.length - 1;
    while (low < high) {
      const mid = (low + high) >> 1;
      if (times[mid]! < t) low = mid + 1;
      else high = mid;
    }
    return low;
  }

  onMount(() => {
    width = container.clientWidth || 600;
    chart = new uPlot(buildOptions(), collect(), container);

    const observer = new ResizeObserver(() => {
      width = container.clientWidth || width;
      chart?.setSize({ width, height });
    });
    observer.observe(container);

    const timer = setInterval(() => chart?.setData(collect()), 250);

    return () => {
      clearInterval(timer);
      observer.disconnect();
      chart?.destroy();
      chart = null;
    };
  });

  // Rebuild when the selected channels change — series count is structural.
  $effect(() => {
    const key = layout.drawn.join(',');
    if (!chart || !key) return;
    chart.destroy();
    chart = new uPlot(buildOptions(), collect(), container);
  });

  const skippedNames = $derived(
    layout.skipped.map((h) => {
      const channel = controller.channelByHandle(h);
      return channel ? `${channel.name} (${channel.unit})` : `#${h}`;
    }),
  );
</script>

<div class="chart" bind:this={container}></div>

{#if skippedNames.length > 0}
  <p class="skipped">
    Not plotted: {skippedNames.join(', ')} — a chart carries at most {MAX_AXES}
    units, and a line without its own axis cannot be read.
  </p>
{/if}

<style>
  .chart { width: 100%; }
  .skipped { margin: 0.4rem 0 0; font-size: 0.72rem; color: var(--warn); }
  :global(.uplot .u-legend) { font-size: 0.75rem; }
</style>
