<script lang="ts">
  // ===========================================================================
  //  DashboardView — VIEW and EDIT (§22–§26).
  //
  //  VIEW is the default and stays out of the way: no grid lines, no handles,
  //  nothing but the measurements.  EDIT is entered deliberately, because an
  //  operator leaning on a tablet at the rig must not be able to rearrange the
  //  screen by accident.
  //
  //  There is no "Save" button, and that is a decision, not an omission: the
  //  layout is saved two seconds after the last change (R4).  What there IS is
  //  an honest indicator — "saving…", "saved", or the error that stopped it.
  // ===========================================================================
  import { onMount } from 'svelte';
  import GridCanvas from '../components/GridCanvas.svelte';
  import WidgetSettings from '../components/WidgetSettings.svelte';
  import { dashboards } from '../lib/dashboards.svelte';
  import { findSlot } from '../lib/layout';
  import { controller } from '../lib/state.svelte';
  import { WIDGET_TYPES, widgetChannels, widgetType } from '../lib/widgets';
  import type { Widget } from '../lib/widgets';

  let editing = $state(false);
  let selectedId = $state('');
  let narrow = $state(false);
  let renaming = $state(false);
  let draftName = $state('');

  const current = $derived(dashboards.current);
  const selected = $derived(
    current?.widgets.find((w) => w.id === selectedId) ?? null,
  );

  // Channels a widget points at that no longer exist, computed in the browser
  // as well as by the firmware: the banner has to name them, and the firmware
  // only reports the first.
  const missing = $derived.by(() => {
    const gone = new Set<string>();
    for (const widget of current?.widgets ?? []) {
      for (const key of widgetChannels(widget)) {
        if (!controller.channelByKey(key)) gone.add(key);
      }
    }
    return [...gone];
  });

  onMount(() => {
    const media = window.matchMedia('(max-width: 700px)');
    narrow = media.matches;
    const onResize = (event: MediaQueryListEvent) => { narrow = event.matches; };
    media.addEventListener('change', onResize);

    void (async () => {
      await dashboards.refreshList();
      if (dashboards.list.length === 0) {
        // A controller that has never had a dashboard gets one, built from
        // whatever the rig actually has.  An empty screen with an "Add widget"
        // button teaches nothing about the instrument.
        await seedFirstDashboard();
      } else {
        await dashboards.open(dashboards.list[0]!.key);
      }
    })();

    // Two seconds is long enough to close a tab in.  An unsaved layout must not
    // depend on the operator waiting politely.
    const flush = () => { void dashboards.flush(); };
    window.addEventListener('beforeunload', flush);
    window.addEventListener('pagehide', flush);

    return () => {
      media.removeEventListener('change', onResize);
      window.removeEventListener('beforeunload', flush);
      window.removeEventListener('pagehide', flush);
      void dashboards.flush();
    };
  });

  async function seedFirstDashboard() {
    const created = await dashboards.create('overview', 'Overview');
    if (!created || !dashboards.current) return;
    const visible = controller.channels.filter((c) => c.visible).slice(0, 6);
    const widgets: Widget[] = [];
    visible.forEach((channel, index) => {
      widgets.push({
        id: `w${index + 1}`, type: 'value',
        x: (index % 4) * 3, y: Math.floor(index / 4) * 2, w: 3, h: 2,
        config: { channel: channel.key },
      });
    });
    if (visible.length > 0) {
      widgets.push({
        id: `w${visible.length + 1}`, type: 'chart',
        x: 0, y: Math.ceil(visible.length / 4) * 2, w: 12, h: 5,
        config: {
          series: visible.slice(0, 3).map((c) => ({ channel: c.key })),
          window_s: 300,
        },
      });
    }
    dashboards.current.widgets = widgets;
    dashboards.touch();
    await dashboards.flush();
  }

  function addWidget(typeId: string) {
    const type = widgetType(typeId);
    if (!type || !current || dashboards.atCapacity()) return;
    const firstFree = controller.channels.find((c) => c.visible)?.key;
    const slot = findSlot(current.widgets, current.grid.columns, type.defaultSize);
    const widget: Widget = {
      id: dashboards.nextWidgetId(),
      type: typeId,
      ...slot,
      config: type.defaults(firstFree),
    };
    current.widgets = [...current.widgets, widget];
    selectedId = widget.id;
    dashboards.touch();
  }

  function removeWidget(id: string) {
    if (!current) return;
    current.widgets = current.widgets.filter((w) => w.id !== id);
    if (selectedId === id) selectedId = '';
    dashboards.touch();
  }

  async function createDashboard() {
    const name = prompt('Name for the new dashboard?');
    if (!name) return;
    const key = name.toLowerCase().replace(/[^a-z0-9]+/g, '_').replace(/^_|_$/g, '')
                    .slice(0, 20) || `board_${dashboards.list.length + 1}`;
    if (await dashboards.create(key, name)) editing = true;
  }

  function startRename() {
    draftName = current?.name ?? '';
    renaming = true;
  }

  function commitRename() {
    if (current && draftName.trim()) {
      current.name = draftName.trim();
      dashboards.touch();
    }
    renaming = false;
  }

  async function removeDashboard() {
    if (!current) return;
    if (!confirm(`Delete the dashboard "${current.name}"? The channels stay.`)) return;
    await dashboards.remove(current.key);
    editing = false;
  }

  function leaveEditing() {
    editing = false;
    selectedId = '';
    void dashboards.flush();
  }
</script>

<div class="page">
  <header class="bar">
    <div class="picker">
      <select value={dashboards.currentKey}
              onchange={(e) => void dashboards.open(e.currentTarget.value)}>
        {#each dashboards.list as entry (entry.key)}
          <option value={entry.key}>
            {entry.name}{entry.dangling_channels ? ' ⚠' : ''}
          </option>
        {/each}
      </select>
      {#if editing}
        <button type="button" onclick={createDashboard}
                disabled={dashboards.list.length >= dashboards.limits.dashboards}
                title={dashboards.list.length >= dashboards.limits.dashboards
                  ? `${dashboards.limits.dashboards} dashboards is this partition's limit`
                  : 'New dashboard'}>+ Dashboard</button>
      {/if}
    </div>

    <div class="state">
      {#if dashboards.error}
        <span class="bad">{dashboards.error}</span>
      {:else if dashboards.saving}
        <span class="muted">saving…</span>
      {:else if dashboards.dirty}
        <span class="muted">unsaved changes</span>
      {:else if editing}
        <span class="ok">saved</span>
      {/if}
    </div>

    <div class="actions">
      {#if editing}
        <button type="button" onclick={startRename}>Rename</button>
        <button type="button" class="danger" onclick={removeDashboard}>Delete</button>
        <button type="button" class="primary" onclick={leaveEditing}>Done</button>
      {:else}
        <button type="button" onclick={() => (editing = true)} disabled={narrow}
                title={narrow ? 'Editing needs a wider screen' : 'Rearrange this dashboard'}>
          Edit layout
        </button>
      {/if}
    </div>
  </header>

  {#if renaming}
    <div class="rename">
      <input type="text" bind:value={draftName} placeholder="Dashboard name"
             onkeydown={(e) => e.key === 'Enter' && commitRename()} />
      <button type="button" class="primary" onclick={commitRename}>Rename</button>
      <button type="button" onclick={() => (renaming = false)}>Cancel</button>
    </div>
  {/if}

  {#if missing.length > 0}
    <!-- A widget whose channel is gone is not removed: the device may simply
         have failed to start this morning, and deleting somebody's layout for
         that would be unforgivable.  It is named instead. -->
    <div class="warning">
      <strong>{missing.length} channel{missing.length === 1 ? '' : 's'} on this dashboard no longer exist:</strong>
      <code>{missing.join(', ')}</code>
      <span class="muted">
        The widgets are kept — check the Hardware page before rebuilding them.
      </span>
    </div>
  {/if}

  {#if editing && !narrow}
    <div class="toolbar">
      <span class="label">Add widget</span>
      {#each WIDGET_TYPES as type (type.id)}
        <button type="button" onclick={() => addWidget(type.id)}
                disabled={dashboards.atCapacity()} title={type.description}>
          {type.name}
        </button>
      {/each}
      <span class="muted small">
        {current?.widgets.length ?? 0} / {dashboards.limits.widgets} widgets
      </span>
    </div>
  {/if}

  {#if dashboards.loading}
    <p class="muted">Loading…</p>
  {:else if !current}
    <div class="empty">
      <h2>No dashboards</h2>
      <p>Add a device on the <strong>Hardware</strong> page, and a dashboard will
         be built from whatever the rig has.</p>
    </div>
  {:else if current.widgets.length === 0}
    <div class="empty">
      <h2>{current.name} is empty</h2>
      <p>
        {#if editing}
          Pick a widget above. Drag tiles to move them, drag the corner to
          resize, arrow keys to nudge.
        {:else}
          Press <strong>Edit layout</strong> to put something on it.
        {/if}
      </p>
    </div>
  {:else}
    <div class="canvas" class:with-panel={editing && selected && !narrow}>
      <GridCanvas
        bind:widgets={current.widgets}
        bind:selectedId
        columns={current.grid.columns}
        rowHeight={current.grid.row_height}
        {editing}
        {narrow}
        onchange={() => dashboards.touch()}
        onedit={(id) => (selectedId = id)}
        onremove={removeWidget} />

      {#if editing && selected && !narrow}
        <WidgetSettings
          bind:widget={current.widgets[current.widgets.findIndex((w) => w.id === selectedId)]}
          onchange={() => dashboards.touch()}
          onclose={() => (selectedId = '')} />
      {/if}
    </div>
  {/if}
</div>

<style>
  .page { display: grid; gap: 0.8rem; align-content: start; }
  .bar { display: flex; align-items: center; gap: 0.7rem; flex-wrap: wrap; }
  .picker { display: flex; gap: 0.4rem; align-items: center; }
  .state { margin-left: auto; font-size: 0.78rem; }
  .actions { display: flex; gap: 0.4rem; }
  .rename { display: flex; gap: 0.4rem; }
  .rename input { flex: 0 1 20rem; }
  .toolbar { display: flex; align-items: center; gap: 0.35rem; flex-wrap: wrap;
             padding: 0.5rem 0.6rem; background: var(--surface); border-radius: 8px;
             border: 1px solid var(--line); }
  .toolbar .label { font-size: 0.7rem; text-transform: uppercase;
                    letter-spacing: 0.05em; color: var(--muted); margin-right: 0.2rem; }
  .canvas { display: grid; gap: 0.8rem; }
  .canvas.with-panel { grid-template-columns: 1fr 17rem; align-items: start; }
  .warning { display: grid; gap: 0.15rem; padding: 0.55rem 0.75rem; border-radius: 7px;
             font-size: 0.8rem; border: 1px solid var(--warn);
             background: color-mix(in srgb, var(--warn) 12%, var(--surface)); }
  .warning code { font-family: ui-monospace, SFMono-Regular, monospace;
                  font-size: 0.76rem; }
  .warning .muted { font-size: 0.73rem; }
  .empty { text-align: center; padding: 3rem 1rem; color: var(--muted); }
  .empty h2 { font-size: 1rem; margin-bottom: 0.4rem; color: var(--text); }
  .empty p { max-width: 34rem; margin: 0 auto; }
  .muted { color: var(--muted); }
  .small { font-size: 0.72rem; }
  .ok { color: var(--ok); }
  .bad { color: var(--danger); }
  select, input { background: var(--surface-2); border: 1px solid var(--line);
                  color: var(--text); border-radius: 6px; padding: 0.28rem 0.45rem;
                  font: inherit; font-size: 0.82rem; }
  button { background: var(--surface-2); border: 1px solid var(--line); color: var(--text);
           border-radius: 6px; padding: 0.28rem 0.65rem; cursor: pointer; font: inherit;
           font-size: 0.78rem; }
  button.primary { background: var(--accent); border-color: var(--accent);
                   color: #05121f; font-weight: 600; }
  button.danger:hover { border-color: var(--danger); color: var(--danger); }
  button:disabled { opacity: 0.5; cursor: default; }

  @media (max-width: 700px) {
    .canvas.with-panel { grid-template-columns: 1fr; }
  }
</style>
