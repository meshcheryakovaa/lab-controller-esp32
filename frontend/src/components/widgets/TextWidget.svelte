<script lang="ts">
  // A label on the layout.  Rendered as text, never as markup: a dashboard is
  // configuration that travels between boards, and configuration must not be
  // able to inject anything into the page that reads it.
  import type { WidgetConfig } from '../../lib/widgets';

  let { config }: { config: WidgetConfig } = $props();
</script>

<div class="note">
  {#if config.title}<strong>{config.title}</strong>{/if}
  {#if config.text}<p>{config.text}</p>{/if}
  {#if !config.title && !config.text}
    <span class="muted">Empty note — open the editor to write something.</span>
  {/if}
</div>

<style>
  .note { height: 100%; overflow: auto; display: grid; align-content: start;
          gap: 0.25rem; font-size: 0.82rem; }
  strong { font-size: 0.78rem; letter-spacing: 0.03em; }
  p { margin: 0; color: var(--muted); white-space: pre-wrap; }
  .muted { color: var(--muted); font-size: 0.75rem; }
</style>
