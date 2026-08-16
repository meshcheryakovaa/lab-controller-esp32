<script lang="ts">
  // A single reading, as large as the tile allows.  Quality is never hidden:
  // a stale or faulted number is styled and labelled as one.
  import { formatValue, qualityClass, qualityLabel } from '../../lib/format';
  import { controller } from '../../lib/state.svelte';
  import type { WidgetConfig } from '../../lib/widgets';
  import MissingChannel from './MissingChannel.svelte';

  let { config }: { config: WidgetConfig } = $props();

  const channel = $derived(config.channel ? controller.channelByKey(config.channel) : undefined);
  const value = $derived(channel ? controller.values[channel.handle] : undefined);
  const quality = $derived(channel ? controller.qualityOf(channel.handle) : 'UNKNOWN');
  const precision = $derived(
    typeof config.precision === 'number' ? config.precision : (channel?.precision ?? 2),
  );
</script>

{#if !channel}
  <MissingChannel key={config.channel} />
{:else}
  <div class="tile">
    <div class="name" title={channel.key}>{config.title || channel.name}</div>
    <div class="reading">
      <span class="value numeric {qualityClass(quality)}">
        {formatValue(value, precision, quality)}
      </span>
      <span class="unit">{channel.unit}</span>
    </div>
    <div class="foot">
      {#if qualityLabel(quality)}
        <span class="flag {qualityClass(quality)}">{qualityLabel(quality)}</span>
      {:else}
        <span class="muted">{channel.key}</span>
      {/if}
    </div>
  </div>
{/if}

<style>
  .tile { height: 100%; display: grid; align-content: center; gap: 0.15rem;
          padding: 0.3rem 0.1rem; min-width: 0; }
  .name { font-size: 0.72rem; color: var(--muted); overflow: hidden;
          text-overflow: ellipsis; white-space: nowrap; }
  .reading { display: flex; align-items: baseline; gap: 0.3rem; min-width: 0; }
  .value { font-size: clamp(1.2rem, 2.6cqw + 0.8rem, 2.6rem); font-weight: 500;
           letter-spacing: -0.01em; overflow: hidden; text-overflow: ellipsis; }
  .unit { font-size: 0.8rem; color: var(--muted); }
  .foot { font-size: 0.68rem; }
  .flag { text-transform: uppercase; letter-spacing: 0.06em; font-weight: 600; }
  .muted { color: var(--muted); }
</style>
