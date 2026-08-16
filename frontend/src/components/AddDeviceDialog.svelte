<script lang="ts">
  // ===========================================================================
  //  AddDeviceDialog — "Add Device" from start to finish.
  //
  //  Every field in step 2 comes from the module's manifest; this file contains
  //  no knowledge of any specific sensor.  Validation is not re-implemented
  //  either: the form calls POST /devices?dry_run=1, which runs exactly the
  //  code path the real create will run.  What you see validated is what will
  //  happen (§60, §63).
  // ===========================================================================
  import { api } from '../lib/api';
  import { ApiRequestError } from '../lib/api';
  import { errorSentence } from '../lib/format';
  import SchemaForm from '../lib/SchemaForm.svelte';
  import { controller, describe } from '../lib/state.svelte';
  import type { ModuleManifest } from '../lib/types';
  import I2cScanPanel from './I2cScanPanel.svelte';

  let {
    open = $bindable(false),
    /** Optional module+address to jump straight to, e.g. from an I²C scan. */
    preset = $bindable<{ module: string; address?: string } | null>(null),
    oncreated = () => {},
  }: {
    open?: boolean;
    preset?: { module: string; address?: string } | null;
    oncreated?: () => void;
  } = $props();

  let step = $state<'pick' | 'configure'>('pick');
  let manifest = $state<ModuleManifest | null>(null);
  let deviceKey = $state('');
  let deviceName = $state('');
  let sampleIntervalMs = $state(100);
  let config = $state<Record<string, unknown>>({});

  let validating = $state(false);
  let validated = $state(false);
  let errorField = $state('');
  let errorText = $state('');
  let creating = $state(false);

  const sensors = $derived(controller.modules.filter((m) => m.category === 'sensor'));
  const outputs = $derived(controller.modules.filter((m) => m.category === 'output'));

  function uniqueKey(moduleId: string): string {
    for (let i = 1; i < 100; ++i) {
      const candidate = `${moduleId}_${String(i).padStart(2, '0')}`;
      if (!controller.devices.some((d) => d.key === candidate)) return candidate;
    }
    return `${moduleId}_xx`;
  }

  function choose(selected: ModuleManifest, presetAddress?: string) {
    manifest = selected;
    deviceKey = uniqueKey(selected.id);
    deviceName = selected.name;
    sampleIntervalMs = Math.round(selected.default_sample_interval_us / 1000);
    config = {};
    if (presetAddress) config.address = presetAddress;
    errorField = '';
    errorText = '';
    validated = false;
    step = 'configure';
  }

  function pickFromScan(moduleId: string, address: string) {
    const found = controller.moduleById(moduleId);
    if (found) choose(found, address);
  }

  // Opened from somewhere that already knows what to add (the Hardware page's
  // bus scanner) — skip the catalogue and land on the configured form.
  $effect(() => {
    if (!open || !preset) return;
    const found = controller.moduleById(preset.module);
    const address = preset.address;
    preset = null;
    if (found) choose(found, address);
  });

  function entry() {
    return {
      key: deviceKey,
      module: manifest?.id,
      name: deviceName,
      sample_interval_us: Math.round(sampleIntervalMs * 1000),
      config,
    };
  }

  // Debounced live validation.  A round trip to the board is cheap; guessing
  // at the rules in the browser is not.
  let validateTimer = 0;
  // Monotonic ticket: a slow reply that is overtaken by a newer one must not
  // be allowed to publish its verdict about a form that has since changed.
  let validateTicket = 0;
  function scheduleValidation() {
    validated = false;
    clearTimeout(validateTimer);
    validateTimer = window.setTimeout(runValidation, 350);
  }

  async function runValidation() {
    if (!manifest || !open) return;
    const ticket = ++validateTicket;
    validating = true;
    errorField = '';
    errorText = '';
    try {
      await api.validateDevice(entry());
      if (ticket !== validateTicket || !open) return;
      validated = true;
    } catch (e) {
      if (ticket !== validateTicket || !open) return;
      validated = false;
      if (e instanceof ApiRequestError) {
        errorField = e.field ?? '';
        errorText = errorSentence(e.error);
      } else {
        errorText = describe(e);
      }
    } finally {
      if (ticket === validateTicket) validating = false;
    }
  }

  // Re-validate whenever anything the firmware would look at changes.
  $effect(() => {
    if (step !== 'configure') return;
    JSON.stringify(config);
    deviceKey;
    sampleIntervalMs;
    scheduleValidation();
  });

  async function create() {
    creating = true;
    try {
      await api.createDevice(entry());
      await controller.refresh();
      close();
      oncreated();
    } catch (e) {
      // The create failed, so the configuration is by definition not valid any
      // more: leaving `validated` set would show "✓ configuration is valid"
      // directly next to the error that just came back.
      validated = false;
      if (e instanceof ApiRequestError) {
        errorField = e.field ?? '';
        errorText = errorSentence(e.error);
      } else {
        errorText = describe(e);
      }
    } finally {
      creating = false;
    }
  }

  function close() {
    // A validation request scheduled a moment ago would otherwise fire against
    // a closed dialog and leave its verdict waiting for the next time it opens.
    clearTimeout(validateTimer);
    validateTimer = 0;
    ++validateTicket;
    validating = false;
    validated = false;
    errorField = '';
    errorText = '';
    open = false;
    step = 'pick';
    manifest = null;
  }
</script>

{#if open}
  <div class="backdrop" role="presentation" onclick={close}></div>
  <div class="dialog" role="dialog" aria-label="Add device">
    <header>
      <h2>{step === 'pick' ? 'Add device' : manifest?.name}</h2>
      <button type="button" class="close" onclick={close} aria-label="Close">×</button>
    </header>

    {#if step === 'pick'}
      <section class="body">
        <p class="hint">
          The list below is the firmware's module catalogue. It is generated from
          the manifests compiled into this build — nothing here is hard-coded in
          the browser.
        </p>

        <h3>Sensors</h3>
        <div class="grid">
          {#each sensors as module (module.id)}
            <button type="button" class="card" onclick={() => choose(module)}>
              <strong>{module.name}</strong>
              <span class="muted">{module.description ?? ''}</span>
              <span class="tag">{module.bus === 'none' ? 'direct' : module.bus.toUpperCase()}</span>
            </button>
          {/each}
        </div>

        {#if outputs.length > 0}
          <h3>Outputs</h3>
          <div class="grid">
            {#each outputs as module (module.id)}
              <button type="button" class="card" onclick={() => choose(module)}>
                <strong>{module.name}</strong>
                <span class="muted">{module.description ?? ''}</span>
              </button>
            {/each}
          </div>
        {/if}

        <h3>Not sure what is connected?</h3>
        <I2cScanPanel bus={0} onpick={pickFromScan} />
      </section>

    {:else if manifest}
      <section class="body">
        <label class="field" class:invalid={errorField === 'key'}>
          <span class="label">Key</span>
          <input type="text" bind:value={deviceKey} />
          {#if errorField === 'key' && errorText}
            <span class="error">{errorText}</span>
          {/if}
          <span class="help">
            Stable identifier used by formulas, dashboards and stored logs. It
            cannot be changed later.
          </span>
        </label>

        <label class="field">
          <span class="label">Name</span>
          <input type="text" bind:value={deviceName} />
        </label>

        <label class="field">
          <span class="label">Sample interval <em class="unit">ms</em></span>
          <input
            type="number"
            min={Math.max(1, Math.round(manifest.min_sample_interval_us / 1000))}
            step="1"
            bind:value={sampleIntervalMs} />
          <span class="help">
            {(1000 / Math.max(1, sampleIntervalMs)).toFixed(1)} Hz — the
            acquisition rate of the sensor itself, unrelated to how often the
            browser redraws.
          </span>
        </label>

        <hr />

        <SchemaForm
          {manifest}
          bind:value={config}
          gpio={controller.gpio}
          {errorField}
          {errorText}
          handledFields={['key']} />

        {#if errorText && !errorField}
          <p class="error">{errorText}</p>
        {/if}
      </section>

      <footer>
        <span class="status">
          {#if validating}
            checking…
          {:else if validated}
            <span class="ok">✓ configuration is valid</span>
          {:else if errorText}
            <span class="bad">✗ {errorField ? `fix ${errorField}` : errorText}</span>
          {/if}
        </span>
        <div class="actions">
          <button type="button" onclick={() => (step = 'pick')}>Back</button>
          <button type="button" class="primary" disabled={!validated || creating} onclick={create}>
            {creating ? 'Creating…' : 'Create'}
          </button>
        </div>
      </footer>
    {/if}
  </div>
{/if}

<style>
  .backdrop { position: fixed; inset: 0; background: rgba(0, 0, 0, 0.55); z-index: 40; }
  .dialog {
    position: fixed; z-index: 41; inset: 4vh 50% auto auto; transform: translateX(50%);
    width: min(680px, 92vw); max-height: 92vh; display: flex; flex-direction: column;
    background: var(--surface); border: 1px solid var(--line); border-radius: 10px;
    box-shadow: 0 20px 60px rgba(0, 0, 0, 0.5);
  }
  header { display: flex; align-items: center; justify-content: space-between;
           padding: 0.9rem 1.1rem; border-bottom: 1px solid var(--line); }
  h2 { margin: 0; font-size: 1rem; font-weight: 600; }
  h3 { margin: 1rem 0 0.4rem; font-size: 0.72rem; text-transform: uppercase;
       letter-spacing: 0.07em; color: var(--muted); font-weight: 600; }
  .close { background: none; border: 0; color: var(--muted); font-size: 1.4rem;
           line-height: 1; cursor: pointer; }
  .body { padding: 1rem 1.1rem; overflow-y: auto; display: grid; gap: 0.7rem; }
  .hint { margin: 0; font-size: 0.8rem; color: var(--muted); }
  .grid { display: grid; grid-template-columns: repeat(auto-fill, minmax(200px, 1fr)); gap: 0.5rem; }
  .card { display: grid; gap: 0.2rem; text-align: left; padding: 0.6rem 0.7rem;
          background: var(--surface-2); border: 1px solid var(--line); border-radius: 7px;
          color: inherit; cursor: pointer; font: inherit; }
  .card:hover { border-color: var(--accent); }
  .card .muted { font-size: 0.72rem; color: var(--muted); }
  .tag { justify-self: start; font-size: 0.62rem; letter-spacing: 0.06em;
         color: var(--muted); border: 1px solid var(--line); border-radius: 3px;
         padding: 0 0.25rem; }
  .field { display: grid; gap: 0.25rem; }
  .field.invalid input { border-color: var(--danger); }
  .label { font-size: 0.72rem; letter-spacing: 0.05em; text-transform: uppercase; opacity: 0.75; }
  .unit { font-style: normal; opacity: 0.55; }
  .help { font-size: 0.72rem; opacity: 0.6; }
  hr { border: 0; border-top: 1px solid var(--line); margin: 0.4rem 0; width: 100%; }
  footer { display: flex; align-items: center; justify-content: space-between;
           gap: 1rem; padding: 0.8rem 1.1rem; border-top: 1px solid var(--line); }
  .status { font-size: 0.8rem; color: var(--muted); }
  .ok { color: var(--ok); }
  .bad { color: var(--danger); }
  .error { color: var(--danger); font-size: 0.8rem; margin: 0; }
  .actions { display: flex; gap: 0.5rem; }
  button.primary { background: var(--accent); border-color: var(--accent); color: #05121f; font-weight: 600; }
  button { background: var(--surface-2); border: 1px solid var(--line); color: var(--text);
           border-radius: 6px; padding: 0.35rem 0.8rem; cursor: pointer; }
  button:disabled { opacity: 0.5; cursor: default; }
</style>
