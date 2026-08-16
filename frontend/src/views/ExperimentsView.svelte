<script lang="ts">
  // ===========================================================================
  //  ExperimentsView — writing a scenario, running it, and what it left behind.
  //
  //  Three panels, in the order the work happens: the run (if there is one),
  //  the scenario being edited, and the history.  The history is not an
  //  afterthought — it is the only screen that answers "what did this rig
  //  actually do", and it shows aborted runs exactly as loudly as finished
  //  ones (§48, ADR-0018).
  //
  //  The step editor is generated from lib/steps.ts.  There is no free-text
  //  field anywhere in it, because there is no step type that takes code.
  // ===========================================================================
  import { onMount } from 'svelte';
  import { api, ApiRequestError } from '../lib/api';
  import { errorSentence } from '../lib/format';
  import { controller, describe } from '../lib/state.svelte';
  import { STEP_TYPES, describeStep, stepType } from '../lib/steps';
  import type { Experiment, ExperimentStep, ExperimentSummary, RunRecord, StepOp } from '../lib/types';

  let summaries = $state<ExperimentSummary[]>([]);
  let records = $state<RunRecord[]>([]);
  let selectedKey = $state('');
  let draft = $state<Experiment | null>(null);
  let dirty = $state(false);
  let busy = $state(false);
  let error = $state('');
  let blocking = $state<{ step: number; reason: string } | null>(null);

  // Run metadata is asked for at start and kept with the record, not with the
  // scenario: the person and the sample are properties of THIS run.
  let operatorName = $state('');
  let sampleName = $state('');

  const run = $derived(controller.run);
  const outputs = $derived(controller.channels.filter((c) => c.direction === 'output'));
  const inputs = $derived(controller.channels.filter((c) => c.direction === 'input'));
  // Same list minus the channels that say they are not data (experiment state).
  const loggable = $derived(inputs.filter((c) => c.logged !== false));
  const loops = $derived(controller.control?.loops ?? []);

  // Every target the firmware will accept, offered as a list rather than as a
  // string somebody has to spell correctly.
  const targets = $derived([
    ...outputs.map((c) => ({ value: c.key, label: `${c.key} — output` })),
    ...loops.flatMap((loop) => [
      { value: `${loop.id}.setpoint`, label: `${loop.id}.setpoint` },
      { value: `${loop.id}.manual`, label: `${loop.id}.manual` },
      { value: `${loop.id}.mode`, label: `${loop.id}.mode` },
    ]),
  ]);
  const loopTargets = $derived([
    ...loops.map((loop) => ({ value: loop.id, label: `${loop.id} — loop` })),
    ...controller.devices.map((d) => ({ value: d.key, label: `${d.key} — device` })),
  ]);

  onMount(() => {
    void reload();
    return controller.watchRun();
  });

  // The run record is written by a background task a moment after the run ends
  // — a flash write inside the control pass would stall the safety pass behind
  // it (ADR-0018).  So the history is re-read once the record can exist;
  // without this the page says "nothing has run yet" directly underneath a run
  // the operator just watched finish.
  let lastState = $state('');
  $effect(() => {
    const state = run?.state ?? '';
    if (state === lastState) return;
    const previous = lastState;
    lastState = state;
    if (previous && (state === 'FINISHED' || state === 'ABORTED')) {
      setTimeout(() => void reload(), 1500);
    }
  });

  async function reload() {
    try {
      const [list, history] = await Promise.all([
        api.experiments(),
        api.runs().catch(() => ({ runs: [] })),
      ]);
      summaries = list.experiments;
      records = history.runs;
      const first = summaries[0];
      if (!selectedKey && first !== undefined) await select(first.key);
    } catch (e) {
      error = describe(e);
    }
  }

  async function select(key: string) {
    if (dirty && !confirm('Discard the unsaved changes to this scenario?')) return;
    selectedKey = key;
    blocking = null;
    try {
      const loaded = await api.experiment(key);
      draft = { ...loaded, metadata: loaded.metadata ?? {}, steps: loaded.steps ?? [] };
      dirty = false;
    } catch (e) {
      error = describe(e);
    }
  }

  function blank() {
    draft = {
      key: `run_${summaries.length + 1}`,
      name: 'New experiment',
      metadata: { description: '' },
      steps: [{ op: 'MARK_EVENT', label: 'started' }],
    };
    selectedKey = '';
    dirty = true;
  }

  function addStep(op: StepOp) {
    if (!draft) return;
    const type = stepType(op);
    draft.steps.push({ op, ...(type?.defaults() ?? {}) } as ExperimentStep);
    dirty = true;
  }

  function move(index: number, delta: number) {
    if (!draft) return;
    const target = index + delta;
    if (target < 0 || target >= draft.steps.length) return;
    const [step] = draft.steps.splice(index, 1);
    if (step === undefined) return;
    draft.steps.splice(target, 0, step);
    dirty = true;
  }

  async function save() {
    if (!draft) return;
    busy = true;
    error = '';
    blocking = null;
    try {
      const result = selectedKey
        ? await api.saveExperiment(selectedKey, draft)
        : await api.createExperiment(draft);
      selectedKey = result.key;
      dirty = false;
      // Saved, but not necessarily runnable: a scenario may name a loop that
      // does not exist yet, and saying so here beats discovering it at start.
      if (!result.runnable) {
        blocking = { step: result.blocking_step ?? 0,
                     reason: result.blocking_reason ?? 'a target does not resolve' };
      }
      await reload();
    } catch (e) {
      error = e instanceof ApiRequestError ? errorSentence(e.error) : describe(e);
    } finally {
      busy = false;
    }
  }

  async function remove() {
    if (!selectedKey || !confirm(`Delete "${selectedKey}"? The run records stay.`)) return;
    busy = true;
    try {
      await api.deleteExperiment(selectedKey);
      selectedKey = '';
      draft = null;
      await reload();
    } catch (e) {
      error = e instanceof ApiRequestError ? errorSentence(e.error) : describe(e);
    } finally {
      busy = false;
    }
  }

  async function act(action: () => Promise<unknown>) {
    busy = true;
    error = '';
    try {
      await action();
      await controller.refreshRun();
      await reload();
    } catch (e) {
      error = e instanceof ApiRequestError ? errorSentence(e.error) : describe(e);
    } finally {
      busy = false;
    }
  }

  function start() {
    if (!selectedKey) return;
    void act(() =>
      api.startExperiment(selectedKey, {
        operator: operatorName,
        sample: sampleName,
      }));
  }

  function when(epochMs: number): string {
    if (!epochMs) return 'clock not set';
    return new Date(epochMs).toLocaleString();
  }

  function duration(seconds: number): string {
    if (seconds < 90) return `${seconds.toFixed(0)} s`;
    if (seconds < 5400) return `${(seconds / 60).toFixed(1)} min`;
    return `${(seconds / 3600).toFixed(2)} h`;
  }
</script>

<div class="page">
  {#if error}<div class="banner bad-banner">{error}</div>{/if}

  <!-- ===================== the run ===================== -->
  <section class="panel">
    <div class="panel-head">
      <h2>Run</h2>
      {#if run}
        <span class="pill {run.state.toLowerCase()}">{run.state}</span>
      {/if}
      <span class="muted small">
        A run holds the rig; stopping it releases every output it commanded and
        switches off every loop it started.
      </span>
    </div>

    <!-- The start form is shown whenever nothing is RUNNING or PAUSED — which
         includes "the last run has ended".  Treating a finished run as a state
         the page has to be rescued from is how a rig ends up with exactly one
         run per reboot. -->
    {#if !controller.runningExperiment}
      {#if run && (run.state === 'FINISHED' || run.state === 'ABORTED')}
        <p class="small last" class:bad={run.state === 'ABORTED'}>
          Last run: <strong>{run.name || run.experiment}</strong> —
          {run.state === 'ABORTED' ? 'aborted' : 'finished'} at step
          {run.step_reached} of {run.steps} · {run.reason}
          {#if run.error}— {run.error.detail || run.error.code}{/if}
        </p>
      {/if}
      <div class="start">
        <label>
          operator
          <input bind:value={operatorName} placeholder="who is running this" />
        </label>
        <label>
          sample
          <input bind:value={sampleName} placeholder="what is in the rig" />
        </label>
        <button type="button" class="primary"
                disabled={busy || !selectedKey || !operatorName || dirty}
                onclick={start}>
          Start {selectedKey || '—'}
        </button>
        {#if dirty}
          <span class="warn small">Save the scenario before running it.</span>
        {:else if !operatorName}
          <span class="muted small">
            The record keeps the operator forever — a dataset nobody can
            attribute is a dataset nobody can ask about.
          </span>
        {/if}
      </div>
    {:else if run}
      <div class="running">
        <div class="what">
          <strong>{run.name || run.experiment}</strong>
          <span class="muted small">
            {run.operator}{run.sample ? ` · ${run.sample}` : ''}
          </span>
        </div>
        <div class="progress">
          <span class="numeric">step {run.step} / {run.steps}</span>
          {#if run.current}
            <span class="muted">
              {run.current.op}
              {run.current.target || run.current.channel || run.current.label || ''}
              {#if run.current.remaining_s}
                · {duration(run.current.remaining_s)} left
              {/if}
            </span>
          {/if}
        </div>
        <div class="actions">
          {#if run.state === 'RUNNING'}
            <button type="button" disabled={busy}
                    onclick={() => act(() => api.pauseExperiment(run.experiment))}>
              Pause
            </button>
          {:else if run.state === 'PAUSED'}
            <button type="button" disabled={busy}
                    onclick={() => act(() => api.resumeExperiment(run.experiment))}>
              Resume
            </button>
          {/if}
          {#if run.state === 'RUNNING' || run.state === 'PAUSED'}
            <button type="button" class="danger" disabled={busy}
                    onclick={() => act(() => api.stopExperiment(run.experiment))}>
              Stop run
            </button>
          {/if}
        </div>
      </div>

      {#if run.events.length > 0}
        <ul class="events">
          {#each run.events as event (event.at_s + event.label)}
            <li>
              <span class="numeric muted">{duration(event.at_s)}</span>
              <span>{event.label}</span>
              <span class="muted small">step {event.step}</span>
            </li>
          {/each}
        </ul>
      {/if}
      {#if run.events_dropped}
        <p class="small warn">
          {run.events_dropped} more events happened than this run can keep.
        </p>
      {/if}
    {/if}
  </section>

  <!-- ===================== the scenario ===================== -->
  <section class="panel">
    <div class="panel-head">
      <h2>Scenario</h2>
      <select value={selectedKey} onchange={(e) => select(e.currentTarget.value)}>
        {#each summaries as summary (summary.key)}
          <option value={summary.key}>{summary.name} ({summary.steps})</option>
        {/each}
        {#if !selectedKey}<option value="">— new —</option>{/if}
      </select>
      <button type="button" onclick={blank}>New</button>
      <button type="button" disabled={!selectedKey || busy || controller.runningExperiment}
              onclick={remove}>Delete</button>
    </div>

    {#if !draft}
      <p class="muted small">No scenario selected.</p>
    {:else}
      <div class="meta">
        <label>key<input bind:value={draft.key} disabled={!!selectedKey}
                         oninput={() => (dirty = true)} /></label>
        <label>name<input bind:value={draft.name} oninput={() => (dirty = true)} /></label>
        <label class="wide">description
          <input bind:value={draft.metadata!.description}
                 oninput={() => (dirty = true)} /></label>
      </div>

      <details class="logging">
        <summary class="small muted">
          Recording — {draft.logging?.channels?.length ?? 0} channels
          {#if draft.logging?.channels?.length}
            at {draft.logging.rate_hz ?? 1} Hz
          {/if}
        </summary>
        <p class="small muted">
          What the scenario records is a property of the scenario; the steps
          START_LOGGING and STOP_LOGGING say when. The dataset carries the
          operator, the sample and the configuration fingerprint of this run.
        </p>
        <div class="fields">
          <label>rate (Hz)
            <input type="number" min="0.1" max="50" step="0.1"
                   value={draft.logging?.rate_hz ?? 1}
                   oninput={(e) => {
                     draft!.logging = { ...(draft!.logging ?? { channels: [] }),
                                        rate_hz: Number(e.currentTarget.value) };
                     dirty = true;
                   }} />
          </label>
          <label class="check">
            <input type="checkbox" checked={draft.logging?.raw ?? true}
                   onchange={(e) => {
                     draft!.logging = { ...(draft!.logging ?? { channels: [] }),
                                        raw: e.currentTarget.checked };
                     dirty = true;
                   }} />
            record raw beside processed
          </label>
        </div>
        <div class="channels">
          {#each loggable as channel (channel.key)}
            <label class="check small">
              <input type="checkbox"
                     checked={draft.logging?.channels?.includes(channel.key) ?? false}
                     onchange={() => {
                       const current = draft!.logging?.channels ?? [];
                       const next = current.includes(channel.key)
                         ? current.filter((k: string) => k !== channel.key)
                         : [...current, channel.key];
                       draft!.logging = { ...(draft!.logging ?? {}), channels: next };
                       dirty = true;
                     }} />
              <span class="numeric">{channel.key}</span>
            </label>
          {/each}
        </div>
      </details>

      {#if blocking}
        <div class="banner warn-banner small">
          Saved, but it will not start yet: step {blocking.step} — {blocking.reason}.
        </div>
      {/if}

      <ol class="steps">
        {#each draft.steps as step, index (index)}
          {@const type = stepType(step.op)}
          <li class:current={controller.runningExperiment && run?.step === index + 1}>
            <div class="step-head">
              <span class="op">{type?.name ?? step.op}</span>
              <span class="muted small">{describeStep(step)}</span>
              <span class="row-actions">
                <button type="button" onclick={() => move(index, -1)} aria-label="Up">↑</button>
                <button type="button" onclick={() => move(index, 1)} aria-label="Down">↓</button>
                <button type="button" onclick={() => {
                  draft!.steps.splice(index, 1);
                  dirty = true;
                }}>Remove</button>
              </span>
            </div>

            {#if type}
              <div class="fields">
                <!-- A SET step addresses either a number or a mode, never both.
                     Showing the irrelevant one invites somebody to fill it in
                     and wonder why it did nothing. -->
                {#each type.fields.filter((f) => step.op !== 'SET' ||
                        (f.key === 'mode') === !!step.target?.endsWith('.mode') ||
                        f.key === 'target') as field (field.key)}
                  <label title={field.help ?? ''}>
                    {field.label}
                    {#if field.kind === 'target'}
                      <select value={step.target ?? ''}
                              onchange={(e) => { step.target = e.currentTarget.value; dirty = true; }}>
                        <option value="">— choose —</option>
                        {#each targets as option (option.value)}
                          <option value={option.value}>{option.label}</option>
                        {/each}
                        {#if step.target && !targets.some((t) => t.value === step.target)}
                          <option value={step.target}>{step.target} (missing)</option>
                        {/if}
                      </select>
                    {:else if field.kind === 'loopTarget'}
                      <select value={step.target ?? ''}
                              onchange={(e) => { step.target = e.currentTarget.value; dirty = true; }}>
                        <option value="">— choose —</option>
                        {#each loopTargets as option (option.value)}
                          <option value={option.value}>{option.label}</option>
                        {/each}
                        {#if step.target && !loopTargets.some((t) => t.value === step.target)}
                          <option value={step.target}>{step.target} (missing)</option>
                        {/if}
                      </select>
                    {:else if field.kind === 'channel'}
                      <select value={step.channel ?? ''}
                              onchange={(e) => { step.channel = e.currentTarget.value; dirty = true; }}>
                        <option value="">— choose —</option>
                        {#each inputs as channel (channel.key)}
                          <option value={channel.key}>{channel.key}</option>
                        {/each}
                        {#if step.channel && !inputs.some((c) => c.key === step.channel)}
                          <option value={step.channel}>{step.channel} (missing)</option>
                        {/if}
                      </select>
                    {:else if field.kind === 'comparison'}
                      <select bind:value={step.comparison} onchange={() => (dirty = true)}>
                        <option value=">=">≥</option>
                        <option value="<=">≤</option>
                        <option value=">">&gt;</option>
                        <option value="<">&lt;</option>
                      </select>
                    {:else if field.kind === 'onTimeout'}
                      <select bind:value={step.on_timeout} onchange={() => (dirty = true)}>
                        <option value="abort">abort the run</option>
                        <option value="continue">carry on</option>
                      </select>
                    {:else if field.kind === 'mode'}
                      <select bind:value={step.mode} onchange={() => (dirty = true)}>
                        <option value="">—</option>
                        <option value="off">off</option>
                        <option value="manual">manual</option>
                        <option value="automatic">automatic</option>
                      </select>
                    {:else if field.kind === 'text'}
                      <input bind:value={step.label} oninput={() => (dirty = true)} />
                    {:else if field.kind === 'seconds'}
                      <input type="number" min="0" step="1"
                             value={(step[field.key] as number) ?? 0}
                             oninput={(e) => {
                               (step as any)[field.key] = Number(e.currentTarget.value);
                               dirty = true;
                             }} />
                    {:else}
                      <input type="number" step="any"
                             value={(step[field.key] as number) ?? 0}
                             oninput={(e) => {
                               (step as any)[field.key] = Number(e.currentTarget.value);
                               dirty = true;
                             }} />
                    {/if}
                  </label>
                {/each}
              </div>
            {:else}
              <p class="small warn">
                This build does not know the step type “{step.op}”. It will not run.
              </p>
            {/if}
          </li>
        {/each}
      </ol>

      <div class="add">
        <span class="muted small">Add a step:</span>
        {#each STEP_TYPES as type (type.op)}
          <button type="button" title={type.description}
                  onclick={() => addStep(type.op)}>{type.name}</button>
        {/each}
      </div>

      <div class="save">
        <button type="button" class="primary" disabled={busy || !dirty} onclick={save}>
          {busy ? 'Saving…' : 'Save scenario'}
        </button>
        {#if controller.runningExperiment}
          <span class="muted small">A run is in progress; edits apply to the next one.</span>
        {/if}
      </div>
    {/if}
  </section>

  <!-- ===================== the history ===================== -->
  <section class="panel">
    <div class="panel-head">
      <h2>Runs</h2>
      <span class="muted small">
        What this rig actually did. An aborted run is kept exactly as loudly as
        one that finished.
      </span>
    </div>

    {#if records.length === 0}
      <p class="muted small">Nothing has run yet.</p>
    {:else}
      <table>
        <thead>
          <tr>
            <th>Started</th><th>Experiment</th><th>Operator</th><th>Sample</th>
            <th class="right">Duration</th><th>Outcome</th><th>Configuration</th>
          </tr>
        </thead>
        <tbody>
          {#each records as record, index (index)}
            <tr>
              <td class="small">{when(record.started_epoch_ms)}</td>
              <td>{record.name || record.experiment}</td>
              <td class="small">{record.operator || '—'}</td>
              <td class="small">{record.sample || '—'}</td>
              <td class="right small numeric">{duration(record.duration_s)}</td>
              <td>
                <span class="pill {record.state.toLowerCase()}">{record.state}</span>
                <span class="muted small">
                  {record.reason} · step {record.step_reached}/{record.steps}
                </span>
                {#if record.error}
                  <div class="small bad">{record.error.detail || record.error.code}</div>
                {/if}
              </td>
              <td class="small muted">
                rev {record.config_revision ?? '—'} · {record.firmware ?? ''}
                {#if record.calibrations?.length}
                  <div>cal: {record.calibrations.join(', ')}</div>
                {/if}
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
  .panel-head > span.muted { flex: 1 1 14rem; }
  h2 { margin: 0; font-size: 0.8rem; text-transform: uppercase;
       letter-spacing: 0.07em; color: var(--muted); font-weight: 600; }
  table { width: 100%; border-collapse: collapse; font-size: 0.85rem; }
  th { text-align: left; font-weight: 500; font-size: 0.68rem; text-transform: uppercase;
       letter-spacing: 0.05em; color: var(--muted); padding: 0 0.6rem 0.35rem 0; }
  th.right, td.right { text-align: right; }
  td { padding: 0.35rem 0.4rem 0.35rem 0; border-top: 1px solid var(--line);
       vertical-align: top; }
  input, select { background: var(--surface-2); border: 1px solid var(--line);
                  color: var(--text); border-radius: 5px; padding: 0.18rem 0.35rem;
                  font: inherit; font-size: 0.8rem; }
  button { background: var(--surface-2); border: 1px solid var(--line);
           color: var(--text); border-radius: 6px; padding: 0.2rem 0.55rem;
           cursor: pointer; font: inherit; font-size: 0.75rem; }
  button:disabled { opacity: 0.5; cursor: default; }
  button.primary { background: var(--accent); border-color: var(--accent);
                   color: #05121f; font-weight: 600; }
  button.danger { color: var(--danger); border-color: var(--danger); }
  .logging { border: 1px solid var(--line); border-radius: 6px;
             padding: 0.4rem 0.5rem; display: grid; gap: 0.3rem; }
  .channels { display: flex; flex-wrap: wrap; gap: 0.5rem; }
  .check { display: inline-flex; align-items: center; gap: 0.3rem; }
  .numeric { font-family: var(--font-mono); }
  .last { border-left: 2px solid var(--muted); padding-left: 0.5rem; }
  .last.bad { border-color: var(--danger); }
  .start, .running, .save, .add, .actions { display: flex; gap: 0.5rem;
           align-items: center; flex-wrap: wrap; }
  .start label, .meta label { display: grid; gap: 0.15rem; font-size: 0.7rem;
           color: var(--muted); }
  .meta { display: grid; gap: 0.4rem; grid-template-columns: 10rem 1fr; }
  .meta .wide { grid-column: 1 / -1; }
  .running { justify-content: space-between; }
  .what { display: grid; }
  .progress { display: grid; }
  .events { list-style: none; margin: 0; padding: 0; display: grid; gap: 0.15rem;
            font-size: 0.78rem; }
  .events li { display: flex; gap: 0.5rem; }
  .steps { margin: 0; padding-left: 1.4rem; display: grid; gap: 0.5rem; }
  .steps li { border: 1px solid var(--line); border-radius: 6px; padding: 0.4rem 0.5rem;
              background: var(--surface-2); }
  .steps li.current { border-color: var(--accent); }
  .step-head { display: flex; align-items: baseline; gap: 0.5rem; }
  .step-head .op { font-weight: 600; font-size: 0.8rem; }
  .step-head .row-actions { margin-left: auto; white-space: nowrap; }
  .row-actions button { margin-left: 0.2rem; }
  .fields { display: grid; gap: 0.4rem; grid-template-columns: repeat(auto-fit, minmax(9rem, 1fr));
            margin-top: 0.35rem; }
  .fields label { display: grid; gap: 0.15rem; font-size: 0.68rem; color: var(--muted); }
  .banner { border-radius: 8px; padding: 0.5rem 0.7rem; font-size: 0.8rem; }
  .bad-banner { background: color-mix(in srgb, var(--danger) 14%, transparent);
                border: 1px solid var(--danger); }
  .warn-banner { background: color-mix(in srgb, var(--warn) 14%, transparent);
                 border: 1px solid var(--warn); }
  .pill { font-size: 0.6rem; text-transform: uppercase; letter-spacing: 0.06em;
          font-weight: 700; border: 1px solid currentColor; border-radius: 3px;
          padding: 0 0.25rem; }
  .pill.running { color: var(--ok); }
  .pill.paused { color: var(--warn); }
  .pill.aborted { color: var(--danger); }
  .pill.finished { color: var(--muted); }
  .pill.idle { color: var(--muted); }
  .muted { color: var(--muted); }
  .small { font-size: 0.72rem; }
  .warn { color: var(--warn); }
  .bad { color: var(--danger); }
  .numeric { font-family: var(--font-mono); }
  p { margin: 0; }
</style>
