<script lang="ts">
  // A gauge answers "where in its range is this?", which a number cannot.
  // It is drawn as an SVG arc rather than pulled from a chart library: the
  // whole thing is forty lines and the bundle budget is 250 KiB for everything.
  import { formatValue, qualityClass, qualityLabel } from '../../lib/format';
  import { controller } from '../../lib/state.svelte';
  import type { WidgetConfig } from '../../lib/widgets';
  import MissingChannel from './MissingChannel.svelte';

  let { config }: { config: WidgetConfig } = $props();

  const channel = $derived(config.channel ? controller.channelByKey(config.channel) : undefined);
  const value = $derived(channel ? controller.values[channel.handle] : undefined);
  const quality = $derived(channel ? controller.qualityOf(channel.handle) : 'UNKNOWN');

  // The channel's own declared range is the default: a gauge whose range was
  // invented in the browser would disagree with the OUT_OF_RANGE flag beside it.
  //
  // And when NEITHER the widget nor the channel declares one, the gauge says so
  // instead of falling back to 0..100.  A needle at three quarters of an
  // invented scale is a statement about the process that nobody made.
  const declared = $derived.by(() => {
    if (typeof config.min === 'number' && typeof config.max === 'number' &&
        config.max > config.min) {
      return { min: config.min, max: config.max };
    }
    if (channel && channel.min < channel.max) {
      return { min: channel.min, max: channel.max };
    }
    return null;
  });
  const min = $derived(declared?.min ?? 0);
  const max = $derived(declared?.max ?? 0);
  const hasRange = $derived(declared !== null);

  const fraction = $derived(
    value === undefined || !hasRange
      ? null
      : Math.min(1, Math.max(0, (value - min) / (max - min))),
  );

  // 240° sweep starting at 150°, i.e. the familiar open-bottom dial.
  const SWEEP = 240;
  const START = 150;

  function arc(from: number, to: number): string {
    const r = 42;
    const p = (deg: number) => {
      const rad = (deg * Math.PI) / 180;
      return [50 + r * Math.cos(rad), 50 + r * Math.sin(rad)];
    };
    const [x1, y1] = p(from);
    const [x2, y2] = p(to);
    const large = to - from > 180 ? 1 : 0;
    return `M ${x1} ${y1} A ${r} ${r} 0 ${large} 1 ${x2} ${y2}`;
  }
</script>

{#if !channel}
  <MissingChannel key={config.channel} />
{:else}
  <div class="gauge">
    <svg viewBox="0 0 100 78" role="img"
         aria-label="{channel.name}: {value ?? 'no value'} {channel.unit}">
      <path d={arc(START, START + SWEEP)} class="track" />
      {#if fraction !== null}
        <path d={arc(START, START + SWEEP * fraction)} class="fill {qualityClass(quality)}" />
      {/if}
    </svg>
    <div class="readout">
      <span class="value numeric {qualityClass(quality)}">
        {formatValue(value, config.precision ?? channel.precision, quality)}
      </span>
      <span class="unit">{channel.unit}</span>
    </div>
    <div class="scale numeric">
      {#if hasRange}
        <span>{min}</span><span class="name">{config.title || channel.name}</span><span>{max}</span>
      {:else}
        <span class="warn">no range declared — set one on this widget</span>
      {/if}
    </div>
    {#if qualityLabel(quality)}
      <div class="flag {qualityClass(quality)}">{qualityLabel(quality)}</div>
    {/if}
  </div>
{/if}

<style>
  .gauge { height: 100%; display: grid; grid-template-rows: 1fr auto auto;
           align-content: center; justify-items: center; gap: 0.1rem; min-height: 0; }
  svg { width: 100%; height: 100%; max-height: 100%; min-height: 0; }
  .track { fill: none; stroke: var(--line); stroke-width: 9; stroke-linecap: round; }
  .fill { fill: none; stroke: var(--accent); stroke-width: 9; stroke-linecap: round; }
  .fill.q-stale { stroke: var(--warn); }
  .fill.q-range, .fill.q-fault { stroke: var(--danger); }
  .readout { display: flex; align-items: baseline; gap: 0.25rem; margin-top: -1.6rem; }
  .value { font-size: clamp(0.95rem, 1.8cqw + 0.6rem, 1.6rem); font-weight: 500; }
  .unit { font-size: 0.72rem; color: var(--muted); }
  .scale { display: flex; gap: 0.5rem; align-items: baseline; font-size: 0.62rem;
           color: var(--muted); max-width: 100%; }
  .scale .name { overflow: hidden; text-overflow: ellipsis; white-space: nowrap; }
  .warn { color: var(--warn); }
  .flag { font-size: 0.62rem; text-transform: uppercase; letter-spacing: 0.06em;
          font-weight: 600; }
</style>
