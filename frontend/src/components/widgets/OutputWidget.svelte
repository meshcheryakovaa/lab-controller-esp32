<script lang="ts">
  // ===========================================================================
  //  OutputWidget — the only control on a dashboard that can start a fire.
  //
  //  Three things are on screen at all times and none of them is optional:
  //  what the actuator is ACTUALLY doing, what its safe state is, and how long
  //  the current command has left.  A slider that shows only the number
  //  somebody typed is a slider that hides a power limit, a stalled fan and an
  //  expiring hold all at once.
  //
  //  While a command is live the widget renews it: a browser that is open and
  //  watching is exactly the evidence the deadline is asking for.  Close the
  //  tab and the renewals stop, which is the point.
  // ===========================================================================
  import { api, ApiRequestError } from '../../lib/api';
  import { errorSentence } from '../../lib/format';
  import { controller, describe } from '../../lib/state.svelte';
  import type { WidgetConfig } from '../../lib/widgets';
  import MissingChannel from './MissingChannel.svelte';

  let { config }: { config: WidgetConfig } = $props();

  const channel = $derived(config.channel ? controller.channelByKey(config.channel) : undefined);
  const output = $derived(channel?.output);
  const value = $derived(channel ? controller.values[channel.handle] : undefined);
  // A switch when the channel has no meaningful range, a slider otherwise.
  const isSwitch = $derived(!!channel && channel.max <= 1.0001);

  let pending = $state<number | null>(null);
  let busy = $state(false);
  let error = $state('');

  const shown = $derived(pending ?? value ?? 0);

  async function send(next: number) {
    if (!channel) return;
    busy = true;
    error = '';
    pending = next;
    try {
      await api.writeChannel(channel.key, next);
      await controller.refreshChannelValues();
    } catch (e) {
      error = e instanceof ApiRequestError ? errorSentence(e.error) : describe(e);
    } finally {
      pending = null;
      busy = false;
    }
  }

  async function release() {
    if (!channel) return;
    busy = true;
    error = '';
    try {
      await api.releaseOutput(channel.key);
      await controller.refreshChannelValues();
    } catch (e) {
      error = e instanceof ApiRequestError ? errorSentence(e.error) : describe(e);
    } finally {
      busy = false;
    }
  }

  // Renew while this tab is open and the command is live.  Deliberately at a
  // third of the hold time: one missed request must not drop the output.
  $effect(() => {
    const key = channel?.key;
    const state = output?.state;
    const hold = output?.hold_s ?? 0;
    if (!key || state !== 'COMMANDED' || hold <= 0) return;

    const period = Math.max(2000, (hold * 1000) / 3);
    const timer = setInterval(() => {
      void api.renewOutput(key).catch(() => {
        // Losing a renewal is not an error worth shouting about: the deadline
        // exists precisely so that silence has a consequence.
      });
    }, period);
    return () => clearInterval(timer);
  });
</script>

{#if !channel}
  <MissingChannel key={config.channel} />
{:else if !output}
  <div class="not-output">
    <strong>Not an output</strong>
    <code>{channel.key}</code>
    <span>This channel reports a measurement; it cannot be commanded.</span>
  </div>
{:else}
  <div class="control">
    <div class="head">
      <span class="name">{config.title || channel.name}</span>
      <span class="state s-{output.state.toLowerCase()}">{output.state.replace('_', ' ')}</span>
    </div>

    <div class="reading">
      <span class="value numeric">{shown.toFixed(channel.precision)}</span>
      <span class="unit">{channel.unit}</span>
    </div>

    {#if isSwitch}
      <div class="row">
        <button type="button" class:on={shown >= 0.5} disabled={busy}
                onclick={() => send(1)}>On</button>
        <button type="button" class:on={shown < 0.5} disabled={busy}
                onclick={() => send(0)}>Off</button>
      </div>
    {:else}
      <input type="range" min={channel.min} max={channel.max} step="1"
             value={shown} disabled={busy}
             onchange={(e) => send(Number(e.currentTarget.value))} />
      <div class="row">
        <button type="button" disabled={busy} onclick={release}>Off</button>
        <span class="muted small">safe: {output.safe_value}{channel.unit}</span>
      </div>
    {/if}

    <div class="foot">
      {#if output.state === 'COMMANDED' && output.expires_in_s !== undefined}
        <span class="muted small"
              title="Renewed automatically while this tab is open. Close it and the output lets go.">
          expires in {Math.round(output.expires_in_s)} s · auto-renewed
        </span>
      {:else if output.state === 'EXPIRED'}
        <span class="warn small">command expired; released to safe</span>
      {:else if output.state === 'DEVICE_FAULT'}
        <span class="bad small">the device carrying this output stopped</span>
      {:else if output.state === 'TRIPPED'}
        <span class="bad small">stopped by the safety layer</span>
      {:else}
        <span class="muted small">safe: {output.safe_value}{channel.unit}</span>
      {/if}
    </div>

    {#if error}<div class="bad small">{error}</div>{/if}
  </div>
{/if}

<style>
  .control { height: 100%; display: grid; align-content: center; gap: 0.25rem;
             min-width: 0; }
  .head { display: flex; align-items: baseline; justify-content: space-between;
          gap: 0.4rem; }
  .name { font-size: 0.74rem; color: var(--muted); overflow: hidden;
          text-overflow: ellipsis; white-space: nowrap; }
  .state { font-size: 0.6rem; text-transform: uppercase; letter-spacing: 0.06em;
           font-weight: 700; }
  .s-commanded { color: var(--accent); }
  .s-safe { color: var(--muted); }
  .s-expired { color: var(--warn); }
  .s-device_fault, .s-tripped { color: var(--danger); }
  .reading { display: flex; align-items: baseline; gap: 0.25rem; }
  .value { font-size: clamp(1.05rem, 2cqw + 0.7rem, 1.9rem); font-weight: 500; }
  .unit { font-size: 0.74rem; color: var(--muted); }
  .row { display: flex; gap: 0.3rem; align-items: center; }
  input[type=range] { width: 100%; accent-color: var(--accent); }
  button { flex: 1 1 0; background: var(--surface-2); border: 1px solid var(--line);
           color: var(--text); border-radius: 6px; padding: 0.2rem 0.4rem;
           cursor: pointer; font: inherit; font-size: 0.75rem; }
  button.on { background: var(--accent); border-color: var(--accent);
              color: #05121f; font-weight: 600; }
  button:disabled { opacity: 0.5; cursor: default; }
  .foot { min-height: 1rem; }
  .small { font-size: 0.66rem; }
  .muted { color: var(--muted); }
  .warn { color: var(--warn); }
  .bad { color: var(--danger); }
  .not-output { height: 100%; display: grid; align-content: center; gap: 0.15rem;
                text-align: center; font-size: 0.75rem; color: var(--warn); }
  .not-output code { font-family: ui-monospace, SFMono-Regular, monospace; }
  .not-output span { color: var(--muted); font-size: 0.68rem; }
</style>
