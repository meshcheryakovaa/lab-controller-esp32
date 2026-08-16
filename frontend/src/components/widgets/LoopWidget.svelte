<script lang="ts">
  // ===========================================================================
  //  LoopWidget — a regulator on a dashboard.
  //
  //  It shows four things and hides none of them: what the sensor says, what
  //  the loop is asking the actuator for, what the setpoint is, and which mode
  //  the loop is in.  A setpoint box on its own would let somebody type 180 and
  //  walk away from a loop that is OFF, or one whose input went stale twenty
  //  minutes ago and is holding nothing.
  //
  //  Unlike the output widget, this one does NOT renew anything.  The loop is
  //  what keeps its own command alive; a browser that renewed on its behalf
  //  would be quietly propping up a controller that had stopped working.
  // ===========================================================================
  import { api, ApiRequestError } from '../../lib/api';
  import { errorSentence } from '../../lib/format';
  import { controller, describe } from '../../lib/state.svelte';
  import type { LoopMode } from '../../lib/types';
  import type { WidgetConfig } from '../../lib/widgets';

  let { config }: { config: WidgetConfig } = $props();

  const loop = $derived(
    config.loop ? controller.control?.loops.find((l) => l.id === config.loop) : undefined,
  );

  // Loops do not travel over the telemetry socket — they are not channels — so
  // a widget that shows one has to ask.  Shared with every other loop widget
  // and with the Control page.
  $effect(() => controller.watchControl());

  let draft = $state<number | null>(null);
  let busy = $state(false);
  let error = $state('');

  const shownSetpoint = $derived(draft ?? loop?.setpoint ?? 0);

  async function act(action: () => Promise<unknown>) {
    busy = true;
    error = '';
    try {
      await action();
      await controller.refreshControl();
      draft = null;
    } catch (e) {
      error = e instanceof ApiRequestError ? errorSentence(e.error) : describe(e);
    } finally {
      busy = false;
    }
  }
</script>

{#if !loop}
  <div class="missing">
    <strong>No such loop</strong>
    <code>{config.loop ?? '—'}</code>
    <span>It was removed from the control configuration, or never applied.</span>
  </div>
{:else}
  <div class="loop">
    <div class="head">
      <span class="name">{config.title || loop.id}</span>
      <span class="state s-{(loop.state ?? 'idle').toLowerCase()}">
        {(loop.state ?? 'IDLE').replace('_', ' ')}
      </span>
    </div>

    <div class="reading">
      <span class="value numeric">
        {loop.measured !== undefined ? loop.measured.toFixed(1) : '—'}
      </span>
      <span class="unit">{loop.unit ?? ''}</span>
      {#if loop.quality && loop.quality !== 'GOOD'}
        <span class="warn small">{loop.quality}</span>
      {/if}
    </div>

    <div class="sp">
      <input type="number" class="numeric" value={shownSetpoint} disabled={busy}
             oninput={(e) => (draft = Number(e.currentTarget.value))} />
      <button type="button" disabled={busy || draft === null}
              onclick={() => act(() => api.setSetpoint(loop.id, shownSetpoint))}>Set</button>
    </div>

    <div class="modes">
      {#each ['off', 'manual', 'automatic'] as const as mode (mode)}
        <button type="button" class:on={loop.mode === mode} disabled={busy}
                onclick={() => act(() => api.setLoopMode(loop.id, mode as LoopMode))}>
          {mode === 'automatic' ? 'auto' : mode}
        </button>
      {/each}
    </div>

    <div class="foot">
      {#if loop.state === 'NO_INPUT'}
        <span class="bad small">input lost — the output was released</span>
      {:else if loop.state === 'BLOCKED'}
        <span class="bad small">{loop.fault?.detail || 'the safety layer refused it'}</span>
      {:else}
        <span class="muted small">
          output {loop.output_applied !== undefined
            ? loop.output_applied.toFixed(0)
            : (loop.output_value?.toFixed(0) ?? '—')}%
          {#if loop.output_value !== undefined && loop.output_applied !== undefined &&
               Math.abs(loop.output_value - loop.output_applied) > 0.05}
            <span class="warn">(asking {loop.output_value.toFixed(0)}%)</span>
          {/if}
          · {loop.output}
        </span>
      {/if}
    </div>

    {#if error}<div class="bad small">{error}</div>{/if}
  </div>
{/if}

<style>
  .loop { height: 100%; display: grid; align-content: center; gap: 0.25rem;
          min-width: 0; }
  .head { display: flex; align-items: baseline; justify-content: space-between;
          gap: 0.4rem; }
  .name { font-size: 0.74rem; color: var(--muted); overflow: hidden;
          text-overflow: ellipsis; white-space: nowrap; }
  .state { font-size: 0.6rem; text-transform: uppercase; letter-spacing: 0.06em;
           font-weight: 700; color: var(--muted); }
  .s-running { color: var(--ok); }
  .s-no_input, .s-blocked { color: var(--danger); }
  .reading { display: flex; align-items: baseline; gap: 0.25rem; }
  .value { font-size: clamp(1.05rem, 2cqw + 0.7rem, 1.9rem); font-weight: 500; }
  .unit { font-size: 0.74rem; color: var(--muted); }
  .sp { display: flex; gap: 0.3rem; }
  .sp input { flex: 1 1 0; min-width: 0; background: var(--surface-2);
              border: 1px solid var(--line); color: var(--text); border-radius: 5px;
              padding: 0.15rem 0.3rem; font: inherit; font-size: 0.78rem;
              text-align: right; }
  .modes { display: flex; gap: 0.25rem; }
  button { flex: 1 1 0; background: var(--surface-2); border: 1px solid var(--line);
           color: var(--text); border-radius: 6px; padding: 0.15rem 0.3rem;
           cursor: pointer; font: inherit; font-size: 0.7rem; }
  button.on { background: var(--accent); border-color: var(--accent);
              color: #05121f; font-weight: 600; }
  button:disabled { opacity: 0.5; cursor: default; }
  .foot { min-height: 1rem; }
  .small { font-size: 0.66rem; }
  .muted { color: var(--muted); }
  .warn { color: var(--warn); }
  .bad { color: var(--danger); }
  .numeric { font-family: var(--font-mono); }
  .missing { height: 100%; display: grid; align-content: center; gap: 0.15rem;
             text-align: center; font-size: 0.75rem; color: var(--warn); }
  .missing code { font-family: var(--font-mono); }
  .missing span { color: var(--muted); font-size: 0.68rem; }
</style>
