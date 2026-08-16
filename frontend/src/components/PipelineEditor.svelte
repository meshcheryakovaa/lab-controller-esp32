<script lang="ts">
  // ===========================================================================
  //  PipelineEditor — the processing chain of one channel (§14).
  //
  //  Like every other form in this application, it is generated from manifests:
  //  the list of available stages is GET /modules filtered to the processing
  //  category, and each stage's settings come from its ParamSpec list.  Adding
  //  a filter to the firmware puts it here with no frontend change.
  //
  //  The calibration stage is shown but NOT editable here.  Its coefficients
  //  live in calibrations.json with their reference points and version history;
  //  letting somebody type a coefficient into this form would create a second,
  //  untraceable way to change what a channel reads.
  // ===========================================================================
  import { api, ApiRequestError } from '../lib/api';
  import { errorSentence } from '../lib/format';
  import SchemaForm from '../lib/SchemaForm.svelte';
  import { controller, describe } from '../lib/state.svelte';
  import type { Channel, ModuleManifest } from '../lib/types';

  let {
    open = $bindable(false),
    channel,
    oncalibrate = () => {},
  }: { open?: boolean; channel: Channel | undefined; oncalibrate?: () => void } = $props();

  interface Stage {
    type: string;
    config: Record<string, unknown>;
  }

  let stages = $state<Stage[]>([]);
  let activeStages = $state<string[]>([]);
  let loading = $state(false);
  let saving = $state(false);
  let error = $state('');
  let message = $state('');
  let adding = $state('');

  const available = $derived(
    controller.modules
      .filter((m) => m.category === 'processing' && m.id !== 'calibration')
      .sort((a, b) => a.name.localeCompare(b.name)),
  );

  const MAX_STAGES = 6;  // limits::kMaxProcessorsPerChannel

  function manifestFor(type: string): ModuleManifest | undefined {
    return controller.moduleById(type);
  }

  $effect(() => {
    if (!open || !channel) return;
    void load();
  });

  async function load() {
    if (!channel) return;
    loading = true;
    error = '';
    message = '';
    try {
      const stored = await api.processing(channel.key);
      const list = (stored.stages ?? []) as Stage[];
      stages = list.map((stage) => ({
        type: stage.type,
        config: { ...(stage.config ?? {}) },
      }));
      activeStages = (stored.active_stages ?? []) as string[];
    } catch (e) {
      error = describe(e);
    } finally {
      loading = false;
    }
  }

  function add(type: string) {
    if (!type || stages.length >= MAX_STAGES) return;
    stages = [...stages, { type, config: {} }];
    adding = '';
  }

  function remove(index: number) {
    stages = stages.filter((_, i) => i !== index);
  }

  function move(index: number, delta: number) {
    const target = index + delta;
    if (target < 0 || target >= stages.length) return;
    const next = [...stages];
    [next[index], next[target]] = [next[target]!, next[index]!];
    stages = next;
  }

  async function save() {
    if (!channel) return;
    saving = true;
    error = '';
    message = '';
    try {
      // Apply-then-persist happens in the firmware: a chain that cannot be
      // built never reaches the file, so it cannot fail again at every boot.
      const result = await api.setProcessing(channel.key, { stages });
      activeStages = result.active_stages;
      message = `${result.active_stages.length} stage(s) running`;
      await controller.refresh();
    } catch (e) {
      error = e instanceof ApiRequestError ? errorSentence(e.error) : describe(e);
    } finally {
      saving = false;
    }
  }

  function close() {
    open = false;
  }
</script>

{#if open && channel}
  <div class="backdrop" role="presentation" onclick={close}></div>
  <div class="dialog" role="dialog" aria-label="Processing pipeline">
    <header>
      <h2>Processing — <span class="numeric">{channel.key}</span></h2>
      <button type="button" class="close" onclick={close} aria-label="Close">×</button>
    </header>

    <section class="body">
      <p class="hint">
        Stages run top to bottom: <span class="numeric">raw</span> goes in at the
        top and <span class="numeric">processed</span> comes out at the bottom.
        Order matters — an average of ADC counts followed by a calibration is not
        the same number as a calibration followed by an average.
      </p>

      {#if loading}
        <p class="muted">Loading…</p>
      {:else}
        {#if stages.length === 0}
          <p class="muted">
            No processing. The channel reports exactly what the sensor produced.
          </p>
        {/if}

        <ol class="stages">
          {#each stages as stage, index (index)}
            <li>
              <div class="head">
                <span class="order numeric">{index + 1}</span>
                <strong>{manifestFor(stage.type)?.name ?? stage.type}</strong>
                <span class="muted small">{manifestFor(stage.type)?.description ?? ''}</span>
                <div class="controls">
                  <button type="button" onclick={() => move(index, -1)}
                          disabled={index === 0} aria-label="Move up">↑</button>
                  <button type="button" onclick={() => move(index, 1)}
                          disabled={index === stages.length - 1} aria-label="Move down">↓</button>
                  <button type="button" class="danger" onclick={() => remove(index)}>Remove</button>
                </div>
              </div>

              {#if stage.type === 'calibration'}
                <!-- Deliberately not a form.  Coefficients belong to a version
                     with its reference points, not to a text box. -->
                <p class="managed">
                  Managed by the calibration editor, with its reference points and
                  version history.
                  <button type="button" class="link" onclick={oncalibrate}>Open calibration…</button>
                </p>
              {:else if manifestFor(stage.type) && stages[index]}
                <SchemaForm
                  manifest={manifestFor(stage.type)!}
                  bind:value={stages[index]!.config}
                  gpio={controller.gpio} />
              {:else}
                <p class="error">
                  This build has no stage called "{stage.type}". It was probably
                  imported from a different firmware; remove it or reflash.
                </p>
              {/if}
            </li>
          {/each}
        </ol>

        <div class="add">
          <select bind:value={adding} disabled={stages.length >= MAX_STAGES}>
            <option value="">Add a stage…</option>
            {#each available as module (module.id)}
              <option value={module.id}>{module.name}</option>
            {/each}
          </select>
          <button type="button" onclick={() => add(adding)}
                  disabled={!adding || stages.length >= MAX_STAGES}>Add</button>
          {#if stages.length >= MAX_STAGES}
            <span class="muted small">
              {MAX_STAGES} stages is the firmware's limit — raising it costs RAM
              on every channel, so it is a deliberate change, not a slider.
            </span>
          {/if}
        </div>

        {#if activeStages.length > 0}
          <p class="note">
            Running now: {activeStages.join(' → ')}
          </p>
        {/if}
      {/if}

      {#if error}<p class="error">{error}</p>{/if}
      {#if message}<p class="note ok">{message}</p>{/if}
    </section>

    <footer>
      <span class="status muted">Saved only when applied successfully.</span>
      <div class="actions">
        <button type="button" onclick={close}>Close</button>
        <button type="button" class="primary" disabled={saving || loading} onclick={save}>
          {saving ? 'Applying…' : 'Apply and save'}
        </button>
      </div>
    </footer>
  </div>
{/if}

<style>
  .backdrop { position: fixed; inset: 0; background: rgba(0, 0, 0, 0.55); z-index: 40; }
  .dialog {
    position: fixed; z-index: 41; inset: 4vh 50% auto auto; transform: translateX(50%);
    width: min(720px, 94vw); max-height: 92vh; overflow-y: auto;
    display: flex; flex-direction: column;
    background: var(--surface); border: 1px solid var(--line); border-radius: 10px;
    box-shadow: 0 20px 60px rgba(0, 0, 0, 0.5);
  }
  header { display: flex; align-items: center; justify-content: space-between;
           padding: 0.9rem 1.1rem; border-bottom: 1px solid var(--line);
           position: sticky; top: 0; background: var(--surface); z-index: 1; }
  h2 { margin: 0; font-size: 1rem; font-weight: 600; }
  .close { background: none; border: 0; color: var(--muted); font-size: 1.4rem;
           line-height: 1; cursor: pointer; }
  .body { padding: 1rem 1.1rem; display: grid; gap: 0.7rem; }
  .hint { margin: 0; font-size: 0.8rem; color: var(--muted); }
  .stages { list-style: none; margin: 0; padding: 0; display: grid; gap: 0.6rem; }
  .stages li { border: 1px solid var(--line); border-radius: 8px; padding: 0.6rem 0.7rem;
               display: grid; gap: 0.5rem; background: var(--surface-2); }
  .head { display: flex; align-items: baseline; gap: 0.5rem; flex-wrap: wrap; }
  .order { width: 1.3rem; height: 1.3rem; border-radius: 50%; background: var(--line);
           display: inline-flex; align-items: center; justify-content: center;
           font-size: 0.7rem; }
  .controls { margin-left: auto; display: flex; gap: 0.25rem; }
  .managed { margin: 0; font-size: 0.78rem; color: var(--muted); }
  .add { display: flex; gap: 0.4rem; align-items: center; flex-wrap: wrap; }
  .add select { flex: 0 1 16rem; }
  select { background: var(--surface-2); border: 1px solid var(--line); color: var(--text);
           border-radius: 6px; padding: 0.3rem 0.45rem; font: inherit; font-size: 0.85rem; }
  .muted { color: var(--muted); }
  .small { font-size: 0.72rem; }
  .error { color: var(--danger); font-size: 0.8rem; margin: 0; }
  .note { font-size: 0.75rem; color: var(--muted); margin: 0; }
  .note.ok { color: var(--ok); }
  footer { display: flex; align-items: center; justify-content: space-between;
           gap: 1rem; padding: 0.8rem 1.1rem; border-top: 1px solid var(--line);
           position: sticky; bottom: 0; background: var(--surface); }
  .status { font-size: 0.78rem; }
  .actions { display: flex; gap: 0.4rem; }
  button { background: var(--surface-2); border: 1px solid var(--line); color: var(--text);
           border-radius: 6px; padding: 0.25rem 0.6rem; cursor: pointer; font: inherit;
           font-size: 0.78rem; }
  button.primary { background: var(--accent); border-color: var(--accent);
                   color: #05121f; font-weight: 600; padding: 0.35rem 0.8rem; }
  button.danger:hover { border-color: var(--danger); color: var(--danger); }
  button.link { background: none; border: 0; color: var(--accent); padding: 0; }
  button:disabled { opacity: 0.5; cursor: default; }
</style>
