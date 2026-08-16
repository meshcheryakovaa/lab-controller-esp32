<script lang="ts">
  // ===========================================================================
  //  WidgetSettings — the settings of one widget, generated from its registry
  //  entry (§24).
  //
  //  The same idea as SchemaForm, one level up: adding a widget type means
  //  adding fields to lib/widgets.ts, not writing a settings panel.  If a
  //  `if (type === 'gauge')` ever appears in this file, the registry is missing
  //  a field kind.
  // ===========================================================================
  import { controller } from '../lib/state.svelte';
  import { widgetType } from '../lib/widgets';
  import type { Widget, WidgetSeries } from '../lib/widgets';

  let {
    widget = $bindable<Widget | null>(null),
    onchange = () => {},
    onclose = () => {},
  }: { widget?: Widget | null; onchange?: () => void; onclose?: () => void } = $props();

  const type = $derived(widget ? widgetType(widget.type) : undefined);
  const channels = $derived(controller.channels.filter((c) => c.visible));

  function setValue(key: string, value: unknown) {
    if (!widget) return;
    if (value === '' || value === null) {
      delete widget.config[key];
    } else {
      widget.config[key] = value;
    }
    onchange();
  }

  function series(): WidgetSeries[] {
    return (widget?.config.series ?? []) as WidgetSeries[];
  }

  function addSeries(channelKey: string) {
    if (!widget || !channelKey) return;
    const existing = series();
    if (existing.some((s) => s.channel === channelKey)) return;
    widget.config.series = [...existing, { channel: channelKey }];
    onchange();
  }

  function removeSeries(channelKey: string) {
    if (!widget) return;
    widget.config.series = series().filter((s) => s.channel !== channelKey);
    onchange();
  }

  let seriesPick = $state('');
</script>

{#if widget && type}
  <aside class="panel">
    <header>
      <div>
        <strong>{type.name}</strong>
        <span class="muted numeric">{widget.id}</span>
      </div>
      <button type="button" class="close" onclick={onclose} aria-label="Close">×</button>
    </header>

    <p class="hint">{type.description}</p>

    <label class="field">
      <span class="label">Title</span>
      <input type="text" value={widget.config.title ?? ''}
             placeholder="uses the channel's name"
             oninput={(e) => setValue('title', e.currentTarget.value)} />
    </label>

    {#each type.fields as field (field.key)}
      {#if field.kind === 'channel'}
        <label class="field">
          <span class="label">{field.label}</span>
          <select value={widget.config[field.key] ?? ''}
                  onchange={(e) => setValue(field.key, e.currentTarget.value)}>
            <option value="">— select channel —</option>
            {#each channels as channel (channel.key)}
              <option value={channel.key}>{channel.name} ({channel.key})</option>
            {/each}
          </select>
          {#if field.help}<span class="help">{field.help}</span>{/if}
        </label>

      {:else if field.kind === 'loop'}
        <label class="field">
          <span class="label">{field.label}</span>
          <select value={widget.config[field.key] ?? ''}
                  onchange={(e) => setValue(field.key, e.currentTarget.value)}>
            <option value="">— select loop —</option>
            {#each controller.control?.loops ?? [] as loop (loop.id)}
              <option value={loop.id}>{loop.id}</option>
            {/each}
          </select>
          {#if field.help}<span class="help">{field.help}</span>{/if}
        </label>

      {:else if field.kind === 'channels'}
        <div class="field">
          <span class="label">{field.label}</span>
          <ul class="series">
            {#each series() as entry (entry.channel)}
              <li>
                <span class="numeric">{entry.channel}</span>
                {#if !controller.channelByKey(entry.channel)}
                  <span class="warn small">no such channel</span>
                {/if}
                <button type="button" class="link"
                        onclick={() => removeSeries(entry.channel)}>remove</button>
              </li>
            {/each}
            {#if series().length === 0}
              <li class="muted small">Nothing plotted yet.</li>
            {/if}
          </ul>
          <div class="row">
            <select bind:value={seriesPick}>
              <option value="">Add a channel…</option>
              {#each channels as channel (channel.key)}
                <option value={channel.key}>{channel.name} ({channel.key})</option>
              {/each}
            </select>
            <button type="button" disabled={!seriesPick}
                    onclick={() => { addSeries(seriesPick); seriesPick = ''; }}>Add</button>
          </div>
          <span class="help">
            Up to three units on one chart; anything beyond that is named under
            it rather than drawn without an axis.
          </span>
        </div>

      {:else if field.kind === 'number'}
        <label class="field">
          <span class="label">{field.label}</span>
          <input type="number" min={field.min} max={field.max}
                 value={widget.config[field.key] ?? ''}
                 oninput={(e) => setValue(field.key,
                    e.currentTarget.value === '' ? '' : Number(e.currentTarget.value))} />
          {#if field.help}<span class="help">{field.help}</span>{/if}
        </label>

      {:else}
        <label class="field">
          <span class="label">{field.label}</span>
          <input type="text" value={widget.config[field.key] ?? ''}
                 oninput={(e) => setValue(field.key, e.currentTarget.value)} />
          {#if field.help}<span class="help">{field.help}</span>{/if}
        </label>
      {/if}
    {/each}

    <p class="note">
      Position and size are set by dragging the tile, or with the arrow keys
      while it has focus.
    </p>
  </aside>
{/if}

<style>
  .panel { background: var(--surface); border: 1px solid var(--line);
           border-radius: 8px; padding: 0.7rem 0.8rem; display: grid; gap: 0.6rem;
           align-content: start; }
  header { display: flex; align-items: center; justify-content: space-between; gap: 0.5rem; }
  header strong { font-size: 0.85rem; }
  .close { background: none; border: 0; color: var(--muted); font-size: 1.2rem;
           line-height: 1; cursor: pointer; }
  .hint { margin: 0; font-size: 0.75rem; color: var(--muted); }
  .field { display: grid; gap: 0.25rem; }
  .label { font-size: 0.7rem; letter-spacing: 0.05em; text-transform: uppercase;
           opacity: 0.75; }
  .help { font-size: 0.7rem; opacity: 0.6; }
  .note { margin: 0; font-size: 0.7rem; color: var(--muted); }
  .row { display: flex; gap: 0.3rem; }
  .series { list-style: none; margin: 0; padding: 0; display: grid; gap: 0.2rem; }
  .series li { display: flex; align-items: baseline; gap: 0.4rem; font-size: 0.78rem; }
  .small { font-size: 0.7rem; }
  .muted { color: var(--muted); }
  .warn { color: var(--warn); }
  input, select { background: var(--surface-2); border: 1px solid var(--line);
                  color: var(--text); border-radius: 6px; padding: 0.28rem 0.4rem;
                  font: inherit; font-size: 0.8rem; width: 100%; }
  button { background: var(--surface-2); border: 1px solid var(--line); color: var(--text);
           border-radius: 6px; padding: 0.25rem 0.55rem; cursor: pointer; font: inherit;
           font-size: 0.75rem; white-space: nowrap; }
  button.link { background: none; border: 0; color: var(--accent); padding: 0;
                margin-left: auto; }
  button:disabled { opacity: 0.5; cursor: default; }
</style>
