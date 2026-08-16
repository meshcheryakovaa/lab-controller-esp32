<script lang="ts">
  import { stateClass } from '../lib/format';

  let { state = 'DISABLED', title = '' }: { state?: string; title?: string } = $props();
</script>

<span class="pill {stateClass(state)}" {title}>{state}</span>

<style>
  .pill {
    display: inline-block;
    padding: 0.1rem 0.45rem;
    border-radius: 3px;
    font-size: 0.68rem;
    letter-spacing: 0.06em;
    font-weight: 600;
    border: 1px solid currentColor;
    white-space: nowrap;
  }
  /* Colour carries meaning only for states that need attention; a healthy rig
     is quiet, so RUNNING is deliberately understated. */
  :global(.s-ok)    { color: var(--ok); }
  :global(.s-warn)  { color: var(--warn); }
  :global(.s-error) { color: var(--danger); }
  :global(.s-busy)  { color: var(--accent); }
  :global(.s-idle)  { color: var(--muted); }
</style>
