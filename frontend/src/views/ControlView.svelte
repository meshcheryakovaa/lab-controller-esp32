<script lang="ts">
  // ===========================================================================
  //  ControlView — the three layers, on one page, in the order that matters.
  //
  //  Interlocks come FIRST and loops second.  That is not a layout preference:
  //  §30 says a safety limit is not a user rule, and a page that puts the PID
  //  at the top and the interlocks at the bottom teaches the opposite.
  //
  //  Two kinds of change live here and they behave differently on purpose:
  //    * OPERATION — mode, setpoint, manual value, resetting a latch.  Takes
  //      effect immediately, one endpoint each, nothing else disturbed.
  //    * CONFIGURATION — adding a loop, changing gains, editing a limit.  Goes
  //      through a whole-document PUT that stops every loop and releases every
  //      output first, and says so before it does it.
  // ===========================================================================
  import { onMount } from 'svelte';
  import { api, ApiRequestError } from '../lib/api';
  import { errorSentence } from '../lib/format';
  import { controller, describe } from '../lib/state.svelte';
  import type {
    ControlDocument, ControlLoop, ControlRule, LoopMode, SafetyLimit,
  } from '../lib/types';

  // The editable copy.  Live state (measured, engaged, latched…) is read from
  // `controller.control`, never from here — a form field that is also a live
  // readout fights the operator's cursor every refresh.
  let draft = $state<ControlDocument | null>(null);
  let dirty = $state(false);
  let saving = $state(false);
  let error = $state('');
  let busyLoop = $state('');

  const live = $derived(controller.control);
  const inputs = $derived(controller.channels.filter((c) => c.direction === 'input'));
  const outputs = $derived(controller.channels.filter((c) => c.direction === 'output'));
  const max = $derived(live?.limits_max ?? { loops: 8, rules: 16, limits: 16 });

  onMount(() => {
    void controller.refreshControl().then(() => reset());
    return controller.watchControl();
  });

  function reset() {
    draft = structuredClone($state.snapshot(controller.control)) as ControlDocument | null;
    if (draft) {
      draft.loops ??= [];
      draft.rules ??= [];
      draft.limits ??= [];
    }
    dirty = false;
    error = '';
  }

  function liveLoop(id: string): ControlLoop | undefined {
    return live?.loops.find((l) => l.id === id);
  }
  function liveRule(id: string): ControlRule | undefined {
    return live?.rules.find((r) => r.id === id);
  }
  function liveLimit(id: string): SafetyLimit | undefined {
    return live?.limits.find((l) => l.id === id);
  }

  /** Configuration only. The firmware ignores live fields; sending them anyway
      would put "state": "RUNNING" into control.json, where it means nothing. */
  function configOnly(document: ControlDocument) {
    return {
      limits: document.limits.map((l) => ({
        id: l.id, channel: l.channel, target: l.target ?? '',
        condition: l.condition, action: l.action,
        low: l.low, high: l.high, for_s: l.for_s,
        require_fresh_input: l.require_fresh_input, enabled: l.enabled,
        message: l.message ?? '',
      })),
      loops: document.loops.map((l) => ({
        id: l.id, input: l.input, output: l.output, setpoint: l.setpoint,
        kp: l.kp, ki: l.ki, kd: l.kd, min: l.min, max: l.max,
        manual: l.manual, invert: l.invert,
        period_s: l.period_s, input_grace_s: l.input_grace_s,
      })),
      rules: document.rules.map((r) => ({
        id: r.id, input: r.input, output: r.output,
        on_above: r.on_above, off_below: r.off_below,
        on_value: r.on_value, off_value: r.off_value,
        min_hold_s: r.min_hold_s, enabled: r.enabled, note: r.note ?? '',
      })),
    };
  }

  const runningLoops = $derived(
    live?.loops.filter((l) => l.mode && l.mode !== 'off').length ?? 0,
  );

  async function save() {
    if (!draft) return;
    // Does this edit take an interlock away?  The firmware asks for the
    // password when it does; asking here means the operator finds out before
    // the request rather than as a 403 (ADR-0020).
    const armed = live?.limits.filter((l) => l.enabled !== false) ?? [];
    const removesProtection = armed.some((limit) =>
      !draft!.limits.some((l) => l.id === limit.id && l.enabled !== false));
    let confirmation: string | undefined;
    if (removesProtection && controller.auth.configured) {
      confirmation = prompt(
        'This removes a safety limit. Confirm with your password:') ?? undefined;
      if (!confirmation) return;
    }
    if (runningLoops > 0 &&
        !confirm(
          `${runningLoops} loop${runningLoops === 1 ? '' : 's'} will be stopped and ` +
          `every output released before the new configuration is applied. Continue?`)) {
      return;
    }
    saving = true;
    error = '';
    try {
      await api.saveControl(
        confirmation ? { ...configOnly(draft), password: confirmation }
                     : configOnly(draft));
      await controller.refreshControl();
      reset();
    } catch (e) {
      error = e instanceof ApiRequestError ? errorSentence(e.error) : describe(e);
    } finally {
      saving = false;
    }
  }

  async function operate(id: string, action: () => Promise<unknown>) {
    busyLoop = id;
    error = '';
    try {
      await action();
      await controller.refreshControl();
    } catch (e) {
      error = e instanceof ApiRequestError ? errorSentence(e.error) : describe(e);
    } finally {
      busyLoop = '';
    }
  }

  function addLoop() {
    if (!draft) return;
    draft.loops.push({
      id: `loop_${draft.loops.length + 1}`,
      input: inputs[0]?.key ?? '', output: outputs[0]?.key ?? '',
      setpoint: 0, kp: 1, ki: 0, kd: 0, min: 0, max: 100,
      manual: 0, invert: false, period_s: 1, input_grace_s: 5,
    });
    dirty = true;
  }

  function addRule() {
    if (!draft) return;
    draft.rules.push({
      id: `rule_${draft.rules.length + 1}`,
      input: inputs[0]?.key ?? '', output: outputs[0]?.key ?? '',
      on_above: 1, off_below: 0, on_value: 1, off_value: 0,
      min_hold_s: 0, enabled: true, note: '',
    });
    dirty = true;
  }

  function addLimit() {
    if (!draft) return;
    draft.limits.push({
      id: `limit_${draft.limits.length + 1}`,
      channel: inputs[0]?.key ?? '', target: '',
      condition: 'above', action: 'trip_all',
      low: 0, high: 100, for_s: 0,
      require_fresh_input: true, enabled: true, message: '',
    });
    dirty = true;
  }

  function stateClass(state?: string): string {
    switch (state) {
      case 'RUNNING': return 'ok';
      case 'NO_INPUT': return 'bad';
      case 'BLOCKED': return 'warn';
      default: return 'muted';
    }
  }
</script>

<div class="page">
  {#if error}
    <div class="banner bad-banner">{error}</div>
  {/if}

  {#if dirty}
    <div class="banner edit-banner">
      <strong>Unsaved changes.</strong>
      <span>
        Applying them stops every running loop and releases every output first —
        gains and thresholds are not changed underneath a regulator that is
        acting on them.
      </span>
      <span class="actions">
        <button type="button" class="primary" disabled={saving} onclick={save}>
          {saving ? 'Applying…' : 'Apply configuration'}
        </button>
        <button type="button" disabled={saving} onclick={reset}>Revert</button>
      </span>
    </div>
  {/if}

  <!-- ===================== interlocks ===================== -->
  <section class="panel">
    <div class="panel-head">
      <h2>Interlocks</h2>
      <span class="muted small">
        Evaluated from the channels, before every regulator. A limit whose sensor
        is stale or faulted trips — that is not configurable.
      </span>
      <button type="button" disabled={(draft?.limits.length ?? 0) >= max.limits}
              onclick={addLimit}>Add interlock</button>
    </div>

    {#if !draft || draft.limits.length === 0}
      <p class="muted small">
        No interlocks. Nothing is watching this rig except the loops themselves,
        and a loop cannot notice that it has gone wrong.
      </p>
    {:else}
      <table>
        <thead>
          <tr>
            <th>Id</th><th>Channel</th><th>Condition</th><th class="right">Low</th>
            <th class="right">High</th><th class="right">For (s)</th>
            <th>Action</th><th>State</th><th></th>
          </tr>
        </thead>
        <tbody>
          {#each draft.limits as limit, index (index)}
            {@const state = liveLimit(limit.id)}
            <tr class:latched={state?.latched}>
              <td>
                <input class="key" bind:value={limit.id} oninput={() => (dirty = true)} />
              </td>
              <td>
                <select bind:value={limit.channel} onchange={() => (dirty = true)}>
                  {#each inputs as channel (channel.key)}
                    <option value={channel.key}>{channel.key}</option>
                  {/each}
                  {#if !inputs.some((c) => c.key === limit.channel)}
                    <option value={limit.channel}>{limit.channel} (missing)</option>
                  {/if}
                </select>
              </td>
              <td>
                <select bind:value={limit.condition} onchange={() => (dirty = true)}>
                  <option value="above">above</option>
                  <option value="below">below</option>
                  <option value="outside">outside</option>
                </select>
              </td>
              <td class="right">
                <input type="number" class="num" bind:value={limit.low}
                       disabled={limit.condition === 'above'}
                       oninput={() => (dirty = true)} />
              </td>
              <td class="right">
                <input type="number" class="num" bind:value={limit.high}
                       disabled={limit.condition === 'below'}
                       oninput={() => (dirty = true)} />
              </td>
              <td class="right">
                <input type="number" class="num" min="0" max="60" step="0.1"
                       bind:value={limit.for_s} oninput={() => (dirty = true)} />
              </td>
              <td>
                <select bind:value={limit.action} onchange={() => (dirty = true)}>
                  <option value="trip_all">stop everything</option>
                  <option value="release_output">release one output</option>
                  <option value="alarm_only">alarm only</option>
                </select>
                {#if limit.action === 'release_output'}
                  <select bind:value={limit.target} onchange={() => (dirty = true)}>
                    <option value="">choose an output…</option>
                    {#each outputs as channel (channel.key)}
                      <option value={channel.key}>{channel.key}</option>
                    {/each}
                  </select>
                {/if}
              </td>
              <td>
                {#if state?.latched}
                  <span class="pill bad">LATCHED</span>
                  {#if state.trips}<span class="muted small"> ×{state.trips}</span>{/if}
                {:else if state?.violating}
                  <span class="pill warn">VIOLATING</span>
                {:else if state && state.channel_present === false}
                  <span class="pill bad">NO CHANNEL</span>
                {:else if state}
                  <span class="pill ok">ARMED</span>
                {:else}
                  <span class="muted small">not applied</span>
                {/if}
              </td>
              <td class="row-actions">
                {#if state?.latched}
                  <button type="button"
                          onclick={() => operate(limit.id, () => api.resetLimit(limit.id))}>
                    Reset
                  </button>
                {/if}
                <button type="button" onclick={() => {
                  draft!.limits.splice(index, 1);
                  dirty = true;
                }}>Remove</button>
              </td>
            </tr>
            {#if state?.latched && state.fault?.detail}
              <tr class="why"><td colspan="9" class="muted small">
                {limit.message || 'tripped'} — {state.fault.detail}.
                Resetting re-arms it; if the cause is still there it trips again
                immediately.
              </td></tr>
            {/if}
          {/each}
        </tbody>
      </table>
      {#if controller.latchedLimits > 0}
        <p class="small">
          <button type="button" onclick={() => operate('*', () => api.resetAllLimits())}>
            Reset every latched interlock
          </button>
          <span class="muted">
            The master stop cannot be cleared while any interlock is latched.
          </span>
        </p>
      {/if}
    {/if}
  </section>

  <!-- ===================== loops ===================== -->
  <section class="panel">
    <div class="panel-head">
      <h2>Loops</h2>
      <span class="muted small">
        PID. Every loop comes up OFF after a reboot, whatever it was doing before.
      </span>
      <button type="button" disabled={(draft?.loops.length ?? 0) >= max.loops}
              onclick={addLoop}>Add loop</button>
    </div>

    {#if !draft || draft.loops.length === 0}
      <p class="muted small">No control loops.</p>
    {:else}
      <div class="loops">
        {#each draft.loops as loop, index (index)}
          {@const state = liveLoop(loop.id)}
          <article class="loop">
            <header>
              <input class="key" bind:value={loop.id} oninput={() => (dirty = true)} />
              <span class="pill {stateClass(state?.state)}">{state?.state ?? 'NOT APPLIED'}</span>
            </header>

            <div class="readout">
              <div>
                <span class="label">measured</span>
                <span class="numeric big">
                  {state?.measured !== undefined ? state.measured.toFixed(2) : '—'}
                </span>
                <span class="unit">{state?.unit ?? ''}</span>
                {#if state?.quality && state.quality !== 'GOOD'}
                  <span class="pill warn">{state.quality}</span>
                {/if}
              </div>
              <div>
                <span class="label">output</span>
                <span class="numeric big">
                  {state?.output_applied !== undefined
                    ? state.output_applied.toFixed(1)
                    : '—'}
                </span>
                <span class="unit">%</span>
                {#if state?.output_value !== undefined &&
                     state.output_applied !== undefined &&
                     Math.abs(state.output_value - state.output_applied) > 0.05}
                  <!-- The loop is asking for one thing and the actuator is doing
                       another: a power limit, a trip, or a device that stopped.
                       Showing only the demand is how a heater at zero reads as
                       94 %. -->
                  <span class="warn small">
                    asking {state.output_value.toFixed(1)}%
                  </span>
                {/if}
              </div>
            </div>

            <div class="setpoint">
              <label for="sp-{index}">setpoint</label>
              <input id="sp-{index}" type="number" class="num" bind:value={loop.setpoint} />
              <button type="button" disabled={busyLoop === loop.id || !state}
                      onclick={() => operate(loop.id, () => api.setSetpoint(loop.id, loop.setpoint))}>
                Set
              </button>
            </div>

            <div class="modes">
              {#each ['off', 'manual', 'automatic'] as const as mode (mode)}
                <button type="button" class:on={state?.mode === mode}
                        disabled={busyLoop === loop.id || !state}
                        onclick={() => operate(loop.id, () => api.setLoopMode(loop.id, mode as LoopMode))}>
                  {mode}
                </button>
              {/each}
            </div>

            {#if state?.mode === 'manual'}
              <div class="setpoint">
                <label for="mv-{index}">manual</label>
                <input id="mv-{index}" type="number" class="num" bind:value={loop.manual} />
                <button type="button" disabled={busyLoop === loop.id}
                        onclick={() => operate(loop.id, () => api.setManualValue(loop.id, loop.manual))}>
                  Send
                </button>
              </div>
            {/if}

            {#if state?.fault?.detail}
              <p class="small bad">{state.fault.detail}</p>
            {/if}
            {#if state && state.input_present === false}
              <p class="small bad">Its input channel <code>{loop.input}</code> does not exist.</p>
            {/if}
            {#if state && state.output_present === false}
              <p class="small bad">Its output channel <code>{loop.output}</code> does not exist.</p>
            {/if}

            <details>
              <summary class="small muted">Tuning and wiring</summary>
              <div class="grid">
                <label>input
                  <select bind:value={loop.input} onchange={() => (dirty = true)}>
                    {#each inputs as channel (channel.key)}
                      <option value={channel.key}>{channel.key}</option>
                    {/each}
                    {#if !inputs.some((c) => c.key === loop.input)}
                      <option value={loop.input}>{loop.input} (missing)</option>
                    {/if}
                  </select>
                </label>
                <label>output
                  <select bind:value={loop.output} onchange={() => (dirty = true)}>
                    {#each outputs as channel (channel.key)}
                      <option value={channel.key}>{channel.key}</option>
                    {/each}
                    {#if !outputs.some((c) => c.key === loop.output)}
                      <option value={loop.output}>{loop.output} (missing)</option>
                    {/if}
                  </select>
                </label>
                <label>Kp<input type="number" step="0.01" bind:value={loop.kp}
                                oninput={() => (dirty = true)} /></label>
                <label>Ki (1/s)<input type="number" step="0.001" bind:value={loop.ki}
                                oninput={() => (dirty = true)} /></label>
                <label>Kd (s)<input type="number" step="0.01" bind:value={loop.kd}
                                oninput={() => (dirty = true)} /></label>
                <label>min %<input type="number" bind:value={loop.min}
                                oninput={() => (dirty = true)} /></label>
                <label>max %<input type="number" bind:value={loop.max}
                                oninput={() => (dirty = true)} /></label>
                <label>period (s)<input type="number" step="0.1" min="0.1"
                                bind:value={loop.period_s} oninput={() => (dirty = true)} /></label>
                <label title="How long a stale or faulted input is tolerated before the output is released.">
                  input grace (s)
                  <input type="number" step="0.5" min="0" max="300"
                         bind:value={loop.input_grace_s} oninput={() => (dirty = true)} />
                </label>
                <label class="check">
                  <input type="checkbox" bind:checked={loop.invert}
                         onchange={() => (dirty = true)} />
                  cooling (more output for a higher measurement)
                </label>
              </div>
              <p class="small muted">
                Integral: {state?.integral?.toFixed(2) ?? '—'} · last error:
                {state?.last_error?.toFixed(2) ?? '—'}
              </p>
              <button type="button" class="danger" onclick={() => {
                draft!.loops.splice(index, 1);
                dirty = true;
              }}>Remove loop</button>
            </details>
          </article>
        {/each}
      </div>
    {/if}
  </section>

  <!-- ===================== rules ===================== -->
  <section class="panel">
    <div class="panel-head">
      <h2>Rules</h2>
      <span class="muted small">
        Convenience automation, not safety. A rule whose input is stale does
        nothing at all — if something must happen when a sensor dies, it belongs
        in an interlock.
      </span>
      <button type="button" disabled={(draft?.rules.length ?? 0) >= max.rules}
              onclick={addRule}>Add rule</button>
    </div>

    {#if !draft || draft.rules.length === 0}
      <p class="muted small">No rules.</p>
    {:else}
      <table>
        <thead>
          <tr>
            <th>Id</th><th>Input</th><th class="right">On above</th>
            <th class="right">Off below</th><th>Output</th>
            <th class="right">On</th><th class="right">Off</th>
            <th class="right">Min hold (s)</th><th>State</th><th></th>
          </tr>
        </thead>
        <tbody>
          {#each draft.rules as rule, index (index)}
            {@const state = liveRule(rule.id)}
            <tr>
              <td><input class="key" bind:value={rule.id} oninput={() => (dirty = true)} /></td>
              <td>
                <select bind:value={rule.input} onchange={() => (dirty = true)}>
                  {#each inputs as channel (channel.key)}
                    <option value={channel.key}>{channel.key}</option>
                  {/each}
                  {#if !inputs.some((c) => c.key === rule.input)}
                    <option value={rule.input}>{rule.input} (missing)</option>
                  {/if}
                </select>
              </td>
              <td class="right"><input type="number" class="num" bind:value={rule.on_above}
                                       oninput={() => (dirty = true)} /></td>
              <td class="right"><input type="number" class="num" bind:value={rule.off_below}
                                       oninput={() => (dirty = true)} /></td>
              <td>
                <select bind:value={rule.output} onchange={() => (dirty = true)}>
                  {#each outputs as channel (channel.key)}
                    <option value={channel.key}>{channel.key}</option>
                  {/each}
                  {#if !outputs.some((c) => c.key === rule.output)}
                    <option value={rule.output}>{rule.output} (missing)</option>
                  {/if}
                </select>
              </td>
              <td class="right"><input type="number" class="num" bind:value={rule.on_value}
                                       oninput={() => (dirty = true)} /></td>
              <td class="right"><input type="number" class="num" bind:value={rule.off_value}
                                       oninput={() => (dirty = true)} /></td>
              <td class="right"><input type="number" class="num" min="0" bind:value={rule.min_hold_s}
                                       oninput={() => (dirty = true)} /></td>
              <td>
                {#if state?.engaged}
                  <span class="pill ok">ENGAGED</span>
                {:else if state}
                  <span class="pill muted">idle</span>
                {:else}
                  <span class="muted small">not applied</span>
                {/if}
                {#if state?.activations}
                  <span class="muted small"> ×{state.activations}</span>
                {/if}
              </td>
              <td class="row-actions">
                <label class="check small">
                  <input type="checkbox" bind:checked={rule.enabled}
                         onchange={() => (dirty = true)} /> on
                </label>
                <button type="button" onclick={() => {
                  draft!.rules.splice(index, 1);
                  dirty = true;
                }}>Remove</button>
              </td>
            </tr>
          {/each}
        </tbody>
      </table>
    {/if}
  </section>
</div>

<style>
  .page { display: grid; gap: 1rem; }
  .panel { background: var(--surface); border: 1px solid var(--line);
           border-radius: 8px; padding: 0.8rem 0.9rem; display: grid; gap: 0.6rem; }
  .panel-head { display: flex; align-items: baseline; gap: 0.6rem; flex-wrap: wrap; }
  .panel-head span { flex: 1 1 16rem; }
  h2 { margin: 0; font-size: 0.8rem; text-transform: uppercase;
       letter-spacing: 0.07em; color: var(--muted); font-weight: 600; }
  table { width: 100%; border-collapse: collapse; font-size: 0.85rem; }
  th { text-align: left; font-weight: 500; font-size: 0.68rem; text-transform: uppercase;
       letter-spacing: 0.05em; color: var(--muted); padding: 0 0.6rem 0.35rem 0; }
  th.right, td.right { text-align: right; }
  td { padding: 0.3rem 0.3rem 0.3rem 0; border-top: 1px solid var(--line);
       vertical-align: top; }
  tr.latched { background: color-mix(in srgb, var(--danger) 12%, transparent); }
  tr.why td { border-top: none; padding-top: 0; }
  input, select { background: var(--surface-2); border: 1px solid var(--line);
                  color: var(--text); border-radius: 5px; padding: 0.15rem 0.3rem;
                  font: inherit; font-size: 0.78rem; }
  input.num { width: 5.5rem; text-align: right; font-family: var(--font-mono); }
  input.key { width: 8rem; font-family: var(--font-mono); }
  input:disabled { opacity: 0.4; }
  button { background: var(--surface-2); border: 1px solid var(--line);
           color: var(--text); border-radius: 6px; padding: 0.2rem 0.55rem;
           cursor: pointer; font: inherit; font-size: 0.75rem; }
  button:disabled { opacity: 0.5; cursor: default; }
  button.primary { background: var(--accent); border-color: var(--accent);
                   color: #05121f; font-weight: 600; }
  button.danger { color: var(--danger); }
  button.on { background: var(--accent); border-color: var(--accent);
              color: #05121f; font-weight: 600; }
  .row-actions { white-space: nowrap; text-align: right; }
  .row-actions button { margin-left: 0.25rem; }
  .banner { border-radius: 8px; padding: 0.6rem 0.8rem; display: grid; gap: 0.3rem;
            font-size: 0.8rem; }
  .edit-banner { background: color-mix(in srgb, var(--warn) 14%, transparent);
                 border: 1px solid var(--warn); }
  .bad-banner { background: color-mix(in srgb, var(--danger) 14%, transparent);
                border: 1px solid var(--danger); }
  .banner .actions { display: flex; gap: 0.4rem; margin-top: 0.2rem; }
  .loops { display: grid; gap: 0.7rem;
           grid-template-columns: repeat(auto-fill, minmax(19rem, 1fr)); }
  .loop { border: 1px solid var(--line); border-radius: 8px; padding: 0.6rem;
          display: grid; gap: 0.45rem; background: var(--surface-2); }
  .loop header { display: flex; align-items: center; justify-content: space-between;
                 gap: 0.5rem; }
  .readout { display: flex; gap: 1.2rem; }
  .readout > div { display: grid; }
  .label { font-size: 0.62rem; text-transform: uppercase; letter-spacing: 0.06em;
           color: var(--muted); }
  .big { font-size: 1.35rem; }
  .unit { font-size: 0.7rem; color: var(--muted); }
  .setpoint { display: flex; align-items: center; gap: 0.4rem; }
  .setpoint label { font-size: 0.7rem; color: var(--muted); width: 4.5rem; }
  .modes { display: flex; gap: 0.3rem; }
  .modes button { flex: 1 1 0; text-transform: capitalize; }
  .grid { display: grid; gap: 0.35rem; grid-template-columns: 1fr 1fr;
          margin: 0.4rem 0; }
  .grid label { display: grid; gap: 0.15rem; font-size: 0.68rem; color: var(--muted); }
  .grid label.check { grid-column: 1 / -1; display: flex; align-items: center;
                      gap: 0.35rem; }
  .check { display: inline-flex; align-items: center; gap: 0.25rem; }
  .pill { font-size: 0.6rem; text-transform: uppercase; letter-spacing: 0.06em;
          font-weight: 700; border: 1px solid currentColor; border-radius: 3px;
          padding: 0 0.25rem; }
  .ok { color: var(--ok); }
  .warn { color: var(--warn); }
  .bad { color: var(--danger); }
  .muted { color: var(--muted); }
  .small { font-size: 0.72rem; }
  .numeric { font-family: var(--font-mono); }
  code { font-family: var(--font-mono); font-size: 0.75rem; }
  details summary { cursor: pointer; }
  p { margin: 0; }
</style>
