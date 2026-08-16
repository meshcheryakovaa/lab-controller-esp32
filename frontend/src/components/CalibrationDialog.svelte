<script lang="ts">
  // ===========================================================================
  //  CalibrationDialog — put weights on the scale, read grams (§12).
  //
  //  Two rules shape this screen:
  //
  //  1. The RAW value is captured by the firmware, not typed by the operator.
  //     Reading 453211 off one page and typing it into another is where
  //     calibrations go wrong, and the mistake is invisible afterwards.
  //
  //  2. The fit is never shown as a single number.  R² of 0.9999 next to one
  //     weight that is out by 0.4 g is not a good calibration, and the average
  //     is exactly what hides it.  Every point's residual is on screen before
  //     anything can be saved.
  //
  //  Nothing is stored until "Save": /calibrations/solve fits and reports and
  //  writes nothing at all.
  // ===========================================================================
  import { api, ApiRequestError } from '../lib/api';
  import { errorSentence } from '../lib/format';
  import { controller, describe } from '../lib/state.svelte';
  import type {
    Calibration, CalibrationFit, CalibrationKind, CalibrationResidual, Channel,
  } from '../lib/types';

  let {
    open = $bindable(false),
    channel,
    onchanged = () => {},
  }: { open?: boolean; channel: Channel | undefined; onchanged?: () => void } = $props();

  const KINDS: Array<{ value: CalibrationKind; label: string; minPoints: number; help: string }> = [
    { value: 'offset', label: 'Offset (tare)', minPoints: 1,
      help: 'Shifts the reading; the slope stays as it is. One point is enough.' },
    { value: 'linear', label: 'Linear', minPoints: 2,
      help: 'y = a + b·x. What almost every load cell and thermocouple needs.' },
    { value: 'poly2', label: 'Quadratic', minPoints: 3,
      help: 'Use when the residuals of a linear fit form a visible curve, not to chase noise.' },
    { value: 'poly3', label: 'Cubic', minPoints: 4,
      help: 'Rarely justified. Four points and a cubic reproduce the points and nothing else.' },
    { value: 'table', label: 'Lookup table', minPoints: 2,
      help: 'Interpolates between the points and clamps outside them; never extrapolates.' },
  ];

  let kind = $state<CalibrationKind>('linear');
  let points = $state<Array<{ raw: number | null; reference: number | null }>>([]);
  let unit = $state('');
  let precision = $state<number>(2);
  let note = $state('');

  let fit = $state<CalibrationFit | null>(null);
  let residuals = $state<CalibrationResidual[]>([]);
  let solving = $state(false);
  let error = $state('');
  let errorField = $state('');
  let saving = $state(false);

  let history = $state<Calibration[]>([]);
  let historyError = $state('');

  const spec = $derived(KINDS.find((k) => k.value === kind)!);
  const filled = $derived(
    points.filter((p) => p.raw !== null && p.reference !== null) as Array<{ raw: number; reference: number }>,
  );
  const enoughPoints = $derived(filled.length >= spec.minPoints);
  const activeRecord = $derived(history.find((h) => h.active));

  // The live reading, so the operator can see the scale settle before capturing.
  const liveRaw = $derived(channel ? controller.channelByKey(channel.key)?.value?.raw : undefined);

  function reset() {
    kind = 'linear';
    points = [{ raw: null, reference: null }, { raw: null, reference: null }];
    unit = channel?.unit ?? '';
    precision = channel?.precision ?? 2;
    note = '';
    fit = null;
    residuals = [];
    error = '';
    errorField = '';
  }

  $effect(() => {
    if (!open || !channel) return;
    reset();
    void loadHistory();
  });

  async function loadHistory() {
    if (!channel) return;
    historyError = '';
    try {
      history = (await api.calibrations(channel.key)).calibrations;
    } catch (e) {
      historyError = describe(e);
    }
  }

  function draft() {
    return {
      channel: channel?.key,
      kind,
      unit: unit || undefined,
      precision,
      note: note || undefined,
      points: filled,
    };
  }

  // Debounced preview.  Fitting is the firmware's job even here: a least
  // squares solve reimplemented in the browser would drift from the one that
  // actually runs, and the operator would be judging the wrong numbers.
  let solveTimer = 0;
  let solveTicket = 0;
  $effect(() => {
    JSON.stringify(points);
    kind;
    if (!open) return;
    clearTimeout(solveTimer);
    solveTimer = window.setTimeout(solve, 300);
  });

  async function solve() {
    if (!channel || !enoughPoints) {
      fit = null;
      residuals = [];
      return;
    }
    const ticket = ++solveTicket;
    solving = true;
    error = '';
    errorField = '';
    try {
      const result = await api.solveCalibration(draft());
      if (ticket !== solveTicket) return;
      fit = result.fit;
      residuals = result.residuals;
    } catch (e) {
      if (ticket !== solveTicket) return;
      fit = null;
      residuals = [];
      if (e instanceof ApiRequestError) {
        errorField = e.field ?? '';
        error = errorSentence(e.error);
      } else {
        error = describe(e);
      }
    } finally {
      if (ticket === solveTicket) solving = false;
    }
  }

  function capture(index: number) {
    const value = liveRaw;
    const row = points[index];
    if (value === undefined || row === undefined) return;
    row.raw = Number(value.toFixed(3));
  }

  function addPoint() {
    points = [...points, { raw: null, reference: null }];
  }

  function removePoint(index: number) {
    points = points.filter((_, i) => i !== index);
  }

  async function save() {
    if (!channel) return;
    saving = true;
    error = '';
    try {
      await api.createCalibration({ ...draft(), activate: true });
      await controller.refresh();
      await loadHistory();
      onchanged();
      reset();
    } catch (e) {
      if (e instanceof ApiRequestError) {
        errorField = e.field ?? '';
        error = errorSentence(e.error);
      } else {
        error = describe(e);
      }
    } finally {
      saving = false;
    }
  }

  async function act(fn: () => Promise<unknown>) {
    historyError = '';
    try {
      await fn();
      await controller.refresh();
      await loadHistory();
      onchanged();
    } catch (e) {
      historyError = describe(e);
    }
  }

  function when(epochMs: number): string {
    if (!epochMs) return 'clock not set';
    return new Date(epochMs).toISOString().slice(0, 16).replace('T', ' ');
  }

  function close() {
    clearTimeout(solveTimer);
    ++solveTicket;
    open = false;
  }
</script>

{#if open && channel}
  <div class="backdrop" role="presentation" onclick={close}></div>
  <div class="dialog" role="dialog" aria-label="Calibrate channel">
    <header>
      <h2>Calibrate <span class="numeric">{channel.key}</span></h2>
      <button type="button" class="close" onclick={close} aria-label="Close">×</button>
    </header>

    <section class="body">
      <p class="hint">
        Put a known value on the sensor, press <strong>Capture</strong> to take
        the raw reading from the controller, then type what it really is. The
        firmware fits the coefficients — nothing here is computed in the browser.
      </p>

      <div class="row">
        <label class="field">
          <span class="label">Fit</span>
          <select bind:value={kind}>
            {#each KINDS as option (option.value)}
              <option value={option.value}>{option.label}</option>
            {/each}
          </select>
          <span class="help">{spec.help}</span>
        </label>
        <label class="field narrow">
          <span class="label">Unit</span>
          <input type="text" bind:value={unit} placeholder="g" />
          <span class="help">Replaces "{channel.unit || 'none'}"</span>
        </label>
        <label class="field narrow">
          <span class="label">Decimals</span>
          <input type="number" min="0" max="6" bind:value={precision} />
        </label>
      </div>

      <div class="points">
        <table>
          <thead>
            <tr>
              <th>Raw</th><th>Is really</th><th>Predicted</th><th>Residual</th><th></th>
            </tr>
          </thead>
          <tbody>
            {#each points as point, index (index)}
              <tr>
                <td>
                  <div class="capture">
                    <input type="number" step="any" bind:value={point.raw} />
                    <button type="button" onclick={() => capture(index)}
                            disabled={liveRaw === undefined}
                            title={liveRaw === undefined
                              ? 'no live reading from this channel'
                              : `capture ${liveRaw}`}>
                      Capture
                    </button>
                  </div>
                </td>
                <td><input type="number" step="any" bind:value={point.reference} /></td>
                <td class="numeric muted">
                  {residuals[index] ? residuals[index].predicted.toFixed(precision) : '—'}
                </td>
                <td class="numeric" class:bad={residuals[index] &&
                       Math.abs(residuals[index].residual) > (fit?.rms_residual ?? 0) * 3 &&
                       Math.abs(residuals[index].residual) > 1e-9}>
                  {residuals[index] ? residuals[index].residual.toFixed(precision + 1) : '—'}
                </td>
                <td>
                  <button type="button" class="link" onclick={() => removePoint(index)}
                          disabled={points.length <= 1}>remove</button>
                </td>
              </tr>
            {/each}
          </tbody>
        </table>
        <button type="button" class="link" onclick={addPoint}>+ add point</button>
      </div>

      <label class="field">
        <span class="label">Note</span>
        <input type="text" bind:value={note}
               placeholder="three weights, 21 °C, operator AM" />
        <span class="help">
          Stored with the version. In six months this is the only thing that
          will explain why the numbers changed.
        </span>
      </label>

      {#if error}
        <p class="error">{errorField ? `${errorField}: ` : ''}{error}</p>
      {/if}

      {#if fit}
        <div class="quality">
          {#if kind === 'table'}
            <span class="muted">
              A lookup table passes through its points exactly, so it has no
              residuals to report. Between points it interpolates; outside them
              it holds the end value instead of extrapolating.
            </span>
          {:else}
            <div class="stat">
              <span class="k">R²</span>
              <span class="v numeric" class:bad={fit.r_squared < 0.999}>
                {fit.r_squared.toFixed(6)}
              </span>
            </div>
            <div class="stat">
              <span class="k">RMS residual</span>
              <span class="v numeric">{fit.rms_residual.toPrecision(3)} {unit}</span>
            </div>
            <div class="stat">
              <span class="k">Worst point</span>
              <span class="v numeric">{fit.max_residual.toPrecision(3)} {unit}</span>
            </div>
          {/if}
        </div>
      {/if}
    </section>

    <footer>
      <span class="status">
        {#if solving}
          fitting…
        {:else if !enoughPoints}
          <span class="muted">{spec.minPoints} points needed for this fit</span>
        {:else if fit}
          <span class="ok">✓ fit computed — nothing saved yet</span>
        {/if}
      </span>
      <div class="actions">
        <button type="button" onclick={close}>Close</button>
        <button type="button" class="primary" disabled={!fit || saving} onclick={save}>
          {saving ? 'Saving…' : 'Save and activate'}
        </button>
      </div>
    </footer>

    <section class="body history">
      <h3>Versions</h3>
      {#if historyError}<p class="error">{historyError}</p>{/if}
      {#if history.length === 0}
        <p class="muted">
          This channel has never been calibrated. Until it is, the reading is
          whatever the sensor produces — {channel.unit || 'raw counts'}.
        </p>
      {:else}
        <table class="versions">
          <thead>
            <tr><th>Version</th><th>Fit</th><th>Points</th><th>RMS</th><th>When</th><th></th></tr>
          </thead>
          <tbody>
            {#each history as record (record.id)}
              <tr class:on={record.active}>
                <td>
                  <strong class="numeric">v{record.version}</strong>
                  {#if record.active}<span class="tag">active</span>{/if}
                  {#if record.note}<div class="muted small">{record.note}</div>{/if}
                </td>
                <td class="muted">{record.kind}{record.unit ? ` → ${record.unit}` : ''}</td>
                <td class="numeric muted">{record.points?.length ?? 0}</td>
                <td class="numeric muted">
                  {record.kind === 'table' ? '—' : record.fit.rms_residual.toPrecision(3)}
                </td>
                <td class="muted small">{when(record.created_epoch_ms)}</td>
                <td class="actions">
                  {#if record.active}
                    <button type="button"
                            onclick={() => act(() => api.deactivateCalibration(record.id))}>
                      Deactivate
                    </button>
                  {:else}
                    <button type="button"
                            onclick={() => act(() => api.activateCalibration(record.id))}>
                      Activate
                    </button>
                    <button type="button" class="danger"
                            onclick={() => act(() => api.deleteCalibration(record.id))}>
                      Delete
                    </button>
                  {/if}
                </td>
              </tr>
            {/each}
          </tbody>
        </table>
        <p class="note">
          Versions are never edited, only added. A dataset recorded under
          {activeRecord ? `v${activeRecord.version}` : 'an earlier version'} stays
          traceable to the numbers that produced it, and rolling back is one click.
        </p>
      {/if}
    </section>
  </div>
{/if}

<style>
  .backdrop { position: fixed; inset: 0; background: rgba(0, 0, 0, 0.55); z-index: 40; }
  .dialog {
    position: fixed; z-index: 41; inset: 3vh 50% auto auto; transform: translateX(50%);
    width: min(820px, 94vw); max-height: 94vh; overflow-y: auto;
    display: flex; flex-direction: column;
    background: var(--surface); border: 1px solid var(--line); border-radius: 10px;
    box-shadow: 0 20px 60px rgba(0, 0, 0, 0.5);
  }
  header { display: flex; align-items: center; justify-content: space-between;
           padding: 0.9rem 1.1rem; border-bottom: 1px solid var(--line);
           position: sticky; top: 0; background: var(--surface); z-index: 1; }
  h2 { margin: 0; font-size: 1rem; font-weight: 600; }
  h3 { margin: 0 0 0.4rem; font-size: 0.72rem; text-transform: uppercase;
       letter-spacing: 0.07em; color: var(--muted); font-weight: 600; }
  .close { background: none; border: 0; color: var(--muted); font-size: 1.4rem;
           line-height: 1; cursor: pointer; }
  .body { padding: 1rem 1.1rem; display: grid; gap: 0.7rem; }
  .history { border-top: 1px solid var(--line); }
  .hint { margin: 0; font-size: 0.8rem; color: var(--muted); }
  .row { display: flex; gap: 0.7rem; flex-wrap: wrap; }
  .field { display: grid; gap: 0.25rem; flex: 1 1 12rem; }
  .field.narrow { flex: 0 0 7rem; }
  .label { font-size: 0.72rem; letter-spacing: 0.05em; text-transform: uppercase; opacity: 0.75; }
  .help { font-size: 0.72rem; opacity: 0.6; }
  table { width: 100%; border-collapse: collapse; font-size: 0.85rem; }
  th { text-align: left; font-weight: 500; font-size: 0.68rem; text-transform: uppercase;
       letter-spacing: 0.05em; color: var(--muted); padding-bottom: 0.3rem; }
  td { padding: 0.25rem 0.4rem 0.25rem 0; border-top: 1px solid var(--line);
       vertical-align: middle; }
  .capture { display: flex; gap: 0.3rem; }
  .capture input { min-width: 7rem; }
  input, select { background: var(--surface-2); border: 1px solid var(--line);
                  color: var(--text); border-radius: 6px; padding: 0.3rem 0.45rem;
                  font: inherit; font-size: 0.85rem; width: 100%; }
  .quality { display: flex; gap: 1.4rem; flex-wrap: wrap; padding: 0.6rem 0.8rem;
             background: var(--surface-2); border-radius: 7px; }
  .stat { display: grid; gap: 0.1rem; }
  .stat .k { font-size: 0.68rem; text-transform: uppercase; letter-spacing: 0.05em;
             color: var(--muted); }
  .stat .v { font-size: 1rem; }
  .bad { color: var(--danger); }
  .ok { color: var(--ok); }
  .muted { color: var(--muted); }
  .small { font-size: 0.72rem; }
  .error { color: var(--danger); font-size: 0.8rem; margin: 0; }
  .note { font-size: 0.75rem; color: var(--muted); margin: 0.3rem 0 0; }
  .tag { font-size: 0.62rem; letter-spacing: 0.06em; color: var(--ok);
         border: 1px solid var(--ok); border-radius: 3px; padding: 0 0.25rem; }
  .versions tr.on { background: color-mix(in srgb, var(--ok) 8%, transparent); }
  footer { display: flex; align-items: center; justify-content: space-between;
           gap: 1rem; padding: 0.8rem 1.1rem; border-top: 1px solid var(--line); }
  .status { font-size: 0.8rem; color: var(--muted); }
  .actions { display: flex; gap: 0.4rem; white-space: nowrap; }
  button { background: var(--surface-2); border: 1px solid var(--line); color: var(--text);
           border-radius: 6px; padding: 0.25rem 0.6rem; cursor: pointer; font: inherit;
           font-size: 0.78rem; }
  button.primary { background: var(--accent); border-color: var(--accent);
                   color: #05121f; font-weight: 600; padding: 0.35rem 0.8rem; }
  button.danger:hover { border-color: var(--danger); color: var(--danger); }
  button.link { background: none; border: 0; color: var(--accent); padding: 0.1rem 0; }
  button:disabled { opacity: 0.5; cursor: default; }
</style>
