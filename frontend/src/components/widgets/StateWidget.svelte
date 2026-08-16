<script lang="ts">
  // Device health on the dashboard, so the operator does not have to be on the
  // Hardware page to learn that a sensor died.  With no device configured it
  // shows everything that is NOT healthy — an empty tile then means "all well",
  // which is the only case where an empty tile is honest.
  import StatusPill from '../StatusPill.svelte';
  import { controller } from '../../lib/state.svelte';
  import type { WidgetConfig } from '../../lib/widgets';

  let { config }: { config: WidgetConfig } = $props();

  const key = $derived(typeof config.device === 'string' ? config.device : '');
  const devices = $derived(
    key
      ? controller.devices.filter((d) => d.key === key)
      : controller.devices.filter((d) => d.state !== 'RUNNING'),
  );
</script>

<div class="states">
  {#if key && devices.length === 0}
    <span class="warn">No device called <code>{key}</code>.</span>
  {:else if devices.length === 0}
    <span class="ok">All devices running.</span>
  {:else}
    {#each devices as device (device.handle)}
      <div class="row">
        <StatusPill state={device.state} />
        <span class="key numeric">{device.key}</span>
        {#if device.error}
          <span class="detail">{device.error.detail || device.error.code}</span>
        {/if}
      </div>
    {/each}
  {/if}
</div>

<style>
  .states { height: 100%; overflow: auto; display: grid; align-content: start;
            gap: 0.3rem; font-size: 0.78rem; }
  .row { display: flex; align-items: center; gap: 0.4rem; flex-wrap: wrap; }
  .key { font-size: 0.75rem; }
  .detail { font-size: 0.7rem; color: var(--danger); }
  .ok { color: var(--ok); font-size: 0.78rem; }
  .warn { color: var(--warn); font-size: 0.78rem; }
  code { font-family: ui-monospace, SFMono-Regular, monospace; }
</style>
