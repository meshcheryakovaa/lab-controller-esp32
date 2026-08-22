<script lang="ts">
  // ===========================================================================
  //  LineChart — uPlot wrapper.
  //
  //  History lives in the browser (see lib/chart-history.ts): the ESP32 has
  //  neither the RAM to keep it nor any reason to re-send it every time a chart
  //  mounts.  It is bounded there, so this component never sees an array that
  //  grows with uptime.
  //
  //  Redraw is throttled to 4 Hz AND conditional.  The eye cannot use more than
  //  four frames a second on a trend line, and rebuilding the series when no
  //  reading has arrived is the loop that made an eight-hour dashboard feel
  //  like a hung instrument.
  // ===========================================================================
  import { onMount } from 'svelte';
  import uPlot from 'uplot';
  import 'uplot/dist/uPlot.min.css';
  import { controller } from '../lib/state.svelte';
  import type { ChartRange } from '../lib/chart-history';

  let {
    handles = [],
    height = 260,
    // Which span to draw.  Chosen by the buttons above the chart; the stored
    // window_s only decides where those buttons start.
    range = 'all',
  }: { handles?: number[]; height?: number; range?: ChartRange } = $props();

  // Three Y axes is the documented readability limit; beyond that the reader
  // cannot tell which axis a line belongs to.
  const MAX_AXES = 3;

  const REDRAW_MS = 250;

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
          // A dropped connection is a hole, not a straight line across it.
          spanGaps: false,
        })),
      ],
    };
  }

  function collect(): uPlot.AlignedData {
    // uPlot wants [x, ...series]; the history hands back the two separately so
    // that the alignment can be tested without uPlot in the room.
    const data = controller.chartData(layout.drawn, range, width);
    return [data.x, ...data.series] as uPlot.AlignedData;
  }

  // What the chart is currently showing.  Redrawing when none of this has moved
  // draws the same picture again, which is all the old unconditional timer did.
  let shownRevision = -1;
  let shownKey = '';
  let shownRange: ChartRange | '' = '';
  let shownWidth = -1;

  function redraw(force = false): void {
    if (!chart) return;
    // A hidden tab still receives telemetry and still fills its history; what
    // it must not do is lay out and paint a canvas nobody is looking at.
    if (!force && typeof document !== 'undefined'
        && document.visibilityState === 'hidden') return;
    const drawn = layout.drawn;
    const revision = controller.historyRevision(drawn);
    const key = drawn.join(',');
    if (!force && revision === shownRevision && key === shownKey
        && range === shownRange && width === shownWidth) return;
    shownRevision = revision;
    shownKey = key;
    shownRange = range;
    shownWidth = width;
    chart.setData(collect());
  }

  onMount(() => {
    width = container.clientWidth || 600;
    chart = new uPlot(buildOptions(), collect(), container);

    const observer = new ResizeObserver(() => {
      width = container.clientWidth || width;
      chart?.setSize({ width, height });
    });
    observer.observe(container);

    const timer = setInterval(() => redraw(), REDRAW_MS);

    // Coming back to the tab is the one moment a full redraw is owed: the
    // history moved on while nothing was being painted.
    const onVisibility = () => {
      if (document.visibilityState === 'visible') redraw(true);
    };
    document.addEventListener('visibilitychange', onVisibility);

    return () => {
      clearInterval(timer);
      document.removeEventListener('visibilitychange', onVisibility);
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
    shownKey = key;
    shownRange = range;
    shownWidth = width;
    shownRevision = controller.historyRevision(layout.drawn);
  });

  // A new range is a new picture, and waiting up to 250 ms for the timer to
  // notice would make the buttons feel broken.
  $effect(() => {
    void range;
    redraw(true);
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
