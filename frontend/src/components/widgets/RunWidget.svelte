<script lang="ts">
  // ===========================================================================
  //  RunWidget — the experiment, on the dashboard the operator is watching.
  //
  //  It exists because the run state is the one thing a dashboard cannot show
  //  by pointing at a channel and still be useful: "state = 1" is true and
  //  useless.  What somebody standing at the rig needs is which scenario is
  //  running, which step it is on, how long that step has left, and a way to
  //  stop it without navigating anywhere.
  // ===========================================================================
  import { api, ApiRequestError } from '../../lib/api';
  import { errorSentence } from '../../lib/format';
  import { controller, describe } from '../../lib/state.svelte';
  import type { WidgetConfig } from '../../lib/widgets';

  let { config }: { config: WidgetConfig } = $props();

  $effect(() => controller.watchRun());

  const run = $derived(controller.run);
  let busy = $state(false);
  let error = $state('');

  async function stop() {
    if (!run) return;
    busy = true;
    error = '';
    try {
      await api.stopExperiment(run.experiment);
      await controller.refreshRun();
    } catch (e) {
      error = e instanceof ApiRequestError ? errorSentence(e.error) : describe(e);
    } finally {
      busy = false;
    }
  }

  function seconds(value: number): string {
    if (value < 90) return `${value.toFixed(0)} s`;
    if (value < 5400) return `${(value / 60).toFixed(1)} min`;
    return `${(value / 3600).toFixed(1)} h`;
  }
</script>

<div class="run">
  <div class="head">
    <span class="name">{config.title || 'Experiment'}</span>
    <span class="state s-{(run?.state ?? 'idle').toLowerCase()}">
      {run?.state ?? 'IDLE'}
    </span>
  </div>

  {#if !run || run.state === 'IDLE'}
    <div class="idle">nothing running</div>
  {:else}
    <div class="what">{run.name || run.experiment}</div>

    {#if run.state === 'RUNNING' || run.state === 'PAUSED'}
      <div class="progress">
        <span class="numeric">{run.step}/{run.steps}</span>
        {#if run.current}
          <span class="muted small">
            {run.current.op}
            {#if run.current.remaining_s}· {seconds(run.current.remaining_s)} left{/if}
          </span>
        {/if}
      </div>
      <!-- The bar is the only decoration here, and it is honest: it is step
           count, not a guess at how long the run has left. -->
      <div class="bar"><div style="width: {(run.step / Math.max(1, run.steps)) * 100}%"></div></div>
      <button type="button" disabled={busy} onclick={stop}>Stop run</button>
    {:else}
      <!-- Ended.  The step it REACHED, not a progress bar at zero: the two say
           very different things about the same run. -->
      <div class="muted small">
        {run.reason} · step {run.step_reached ?? 0} of {run.steps}
      </div>
    {/if}
  {/if}

  {#if error}<div class="bad small">{error}</div>{/if}
</div>

<style>
  .run { height: 100%; display: grid; align-content: center; gap: 0.25rem;
         min-width: 0; }
  .head { display: flex; align-items: baseline; justify-content: space-between;
          gap: 0.4rem; }
  .name { font-size: 0.74rem; color: var(--muted); overflow: hidden;
          text-overflow: ellipsis; white-space: nowrap; }
  .state { font-size: 0.6rem; text-transform: uppercase; letter-spacing: 0.06em;
           font-weight: 700; color: var(--muted); }
  .s-running { color: var(--ok); }
  .s-paused { color: var(--warn); }
  .s-aborted { color: var(--danger); }
  .what { font-size: 0.95rem; overflow: hidden; text-overflow: ellipsis;
          white-space: nowrap; }
  .idle { font-size: 0.8rem; color: var(--muted); }
  .progress { display: flex; gap: 0.4rem; align-items: baseline; }
  .bar { height: 4px; background: var(--surface-2); border-radius: 2px;
         overflow: hidden; }
  .bar div { height: 100%; background: var(--accent); }
  button { background: var(--surface-2); border: 1px solid var(--danger);
           color: var(--danger); border-radius: 6px; padding: 0.15rem 0.4rem;
           cursor: pointer; font: inherit; font-size: 0.7rem; }
  button:disabled { opacity: 0.5; cursor: default; }
  .muted { color: var(--muted); }
  .small { font-size: 0.66rem; }
  .bad { color: var(--danger); }
  .numeric { font-family: var(--font-mono); }
</style>
