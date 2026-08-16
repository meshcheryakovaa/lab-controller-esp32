<script lang="ts">
  // A widget whose channel is gone must SAY so.  Rendering an empty box would
  // read as "the sensor is quiet", which is the one thing it is not.
  let { key }: { key?: string } = $props();
</script>

<div class="missing">
  <strong>Channel missing</strong>
  {#if key}<code>{key}</code>{:else}<span class="muted">no channel selected</span>{/if}
  <span class="muted">The device may have been removed or failed to start.</span>
</div>

<style>
  .missing { height: 100%; display: grid; align-content: center; gap: 0.15rem;
             font-size: 0.75rem; color: var(--warn); text-align: center;
             padding: 0.3rem; }
  code { font-family: ui-monospace, SFMono-Regular, monospace; font-size: 0.72rem; }
  .muted { color: var(--muted); font-size: 0.68rem; }
</style>
