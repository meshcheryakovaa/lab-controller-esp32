<script lang="ts">
  // ===========================================================================
  //  LocalSessionChart — looking at a finished local recording (M14 §16).
  //
  //  A stored session may be gigabytes, so this never loads one.  It asks the
  //  worker for as many buckets as the widget has pixels, and the worker walks
  //  the chunks of just the visible range.  Zooming re-asks; it does not filter
  //  something already in memory, because there is nothing in memory to filter.
  //
  //  The live socket does NOT feed this chart.  A historical view that quietly
  //  grew a live tail would make the recording look like it was still running.
  // ===========================================================================
  import { onMount } from 'svelte';
  import uPlot from 'uplot';
  import 'uplot/dist/uPlot.min.css';
  import type { LocalEvent, LocalSession } from '../lib/local-history-types';
  import { readLocalSeries } from '../lib/local-history-client';

  let {
    session,
    events = [] as LocalEvent[],
    height = 320,
  }: { session: LocalSession; events?: LocalEvent[]; height?: number } = $props();

  const MAX_SERIES = 4;

  let container: HTMLDivElement;
  let chart: uPlot | null = null;
  let width = $state(700);
  let loading = $state(false);
  let error = $state('');
  let emptyBuckets = $state(0);
  let rowsRead = $state(0);

  const fullFrom = $derived(session.startedClientEpochMs);
  const fullTo = $derived(session.endedClientEpochMs ?? Date.now());

  // null means "the whole session", resolved against the derived bounds — so
  // opening a different session does not keep the previous one's window.
  let from = $state<number | null>(null);
  let to = $state<number | null>(null);
  const viewFrom = $derived(from ?? fullFrom);
  const viewTo = $derived(to ?? fullTo);

  let picked = $state<string[] | null>(null);
  const series = $derived(
    picked ?? session.channels.slice(0, MAX_SERIES).map((c) => c.key));

  const marks = $derived(events.filter((e) => e.type === 'MARK'));

  function options(keys: string[]): uPlot.Options {
    const palette = ['#4c9aff', '#ff8a3d', '#3fb950', '#d29922'];
    return {
      width,
      height,
      cursor: { drag: { x: true, y: false } },
      legend: { live: true },
      scales: { x: { time: true } },
      axes: [
        { stroke: '#7d8b9a', grid: { stroke: '#1b242f' } },
        { stroke: '#7d8b9a', grid: { stroke: '#1b242f' } },
      ],
      series: [
        {},
        ...keys.map((key, i) => {
          const channel = session.channels.find((c) => c.key === key);
          return {
            label: channel ? `${channel.name} (${channel.unit})` : key,
            stroke: palette[i % palette.length],
            width: 1.5,
            // A bucket with no rows in it is a hole in the recording, and a
            // line drawn across it would be a measurement nobody took.
            spanGaps: false,
          };
        }),
      ],
    };
  }

  async function load() {
    if (series.length === 0) return;
    loading = true;
    error = '';
    try {
      const buckets = Math.max(200, Math.round(width));
      const read = await readLocalSeries(session.id, series, viewFrom, viewTo, buckets);
      emptyBuckets = read.emptyBuckets;
      rowsRead = read.rowsRead;
      // uPlot wants seconds on a time scale.
      const x = read.time.map((t) => t / 1000);
      const data = [x, ...read.last] as unknown as uPlot.AlignedData;
      if (chart) chart.destroy();
      chart = new uPlot(options(series), data, container);
    } catch (e) {
      error = e instanceof Error ? e.message : String(e);
    } finally {
      loading = false;
    }
  }

  onMount(() => {
    width = container.clientWidth || 700;
    void load();
    const observer = new ResizeObserver(() => {
      const next = container.clientWidth || width;
      if (Math.abs(next - width) < 40) return;   // re-reading is not free
      width = next;
      void load();
    });
    observer.observe(container);
    return () => {
      observer.disconnect();
      chart?.destroy();
      chart = null;
    };
  });

  function toggle(key: string) {
    picked = series.includes(key)
      ? series.filter((k) => k !== key)
      : [...series, key].slice(0, MAX_SERIES);
    void load();
  }

  function zoomAll() {
    from = null;
    to = null;
    void load();
  }

  function zoomTo(fraction: number) {
    const span = (fullTo - fullFrom) * fraction;
    to = fullTo;
    from = Math.max(fullFrom, fullTo - span);
    void load();
  }
</script>

<div class="wrap">
  <div class="controls">
    <span class="muted small">Range</span>
    <button type="button" onclick={zoomAll}>Whole session</button>
    <button type="button" onclick={() => zoomTo(0.5)}>Last half</button>
    <button type="button" onclick={() => zoomTo(0.1)}>Last tenth</button>
    <span class="muted small">
      {new Date(viewFrom).toLocaleString()} — {new Date(viewTo).toLocaleString()}
    </span>
  </div>

  <div class="controls">
    <span class="muted small">Channels (up to {MAX_SERIES})</span>
    {#each session.channels as channel (channel.key)}
      <button type="button" class:on={series.includes(channel.key)}
              aria-pressed={series.includes(channel.key)}
              onclick={() => toggle(channel.key)}>{channel.key}</button>
    {/each}
  </div>

  <div class="chart" bind:this={container}></div>

  {#if loading}<p class="muted small">Reading…</p>{/if}
  {#if error}<p class="bad small">{error}</p>{/if}

  <p class="muted small">
    {rowsRead.toLocaleString()} rows read into {Math.max(200, Math.round(width))} buckets
    {#if emptyBuckets > 0}
      · <span class="warn">{emptyBuckets} empty buckets — periods this device
      recorded nothing</span>
    {/if}
    {#if marks.length > 0}
      · {marks.length} marked event{marks.length === 1 ? '' : 's'}
    {/if}
  </p>

  {#if marks.length > 0}
    <ul class="marks">
      {#each marks as mark (mark.sequence)}
        <li>
          <span class="numeric">{new Date(mark.clientEpochMs).toLocaleTimeString()}</span>
          <span>{mark.label}</span>
        </li>
      {/each}
    </ul>
  {/if}
</div>

<style>
  .wrap { display: grid; gap: 0.4rem; }
  .chart { width: 100%; }
  .controls { display: flex; gap: 0.3rem; align-items: center; flex-wrap: wrap; }
  .controls button { background: var(--surface-2); border: 1px solid var(--line);
                     color: var(--muted); border-radius: 5px; padding: 0.15rem 0.45rem;
                     font: inherit; font-size: 0.7rem; cursor: pointer; }
  .controls button.on { background: var(--accent); border-color: var(--accent);
                        color: #06121f; font-weight: 600; }
  .muted { color: var(--muted); }
  .warn { color: var(--warn); }
  .bad { color: var(--danger); }
  .small { font-size: 0.72rem; }
  .numeric { font-family: var(--font-mono); }
  .marks { list-style: none; margin: 0; padding: 0; display: grid; gap: 0.15rem;
           font-size: 0.75rem; }
  .marks li { display: flex; gap: 0.5rem; }
  :global(.uplot .u-legend) { font-size: 0.75rem; }
</style>
