<script lang="ts">
  import { onMount } from 'svelte';
  import CalibrationDialog from '../components/CalibrationDialog.svelte';
  import PipelineEditor from '../components/PipelineEditor.svelte';
  import { formatValue, qualityClass, qualityLabel } from '../lib/format';
  import { controller } from '../lib/state.svelte';
  import type { Channel } from '../lib/types';

  let calibrating = $state(false);
  let editingPipeline = $state(false);
  let target = $state<Channel | undefined>(undefined);

  function calibrate(channel: Channel) {
    target = channel;
    editingPipeline = false;
    calibrating = true;
  }

  function editPipeline(channel: Channel) {
    target = channel;
    calibrating = false;
    editingPipeline = true;
  }

  // The selection is held by KEY, not by object or handle.  Handles are reused
  // slots: keeping the object would leave the detail panel describing a deleted
  // channel while the highlighted row is a new one that landed in the same slot.
  let selectedKey = $state('');
  const selected = $derived(selectedKey ? controller.channelByKey(selectedKey) : undefined);

  const rows = $derived(controller.channels);

  // Raw and calibrated exist only in the REST snapshot — the socket carries
  // processed values.  While this page is open, re-read them so the three
  // columns describe the same instant.
  onMount(() => {
    void controller.refreshChannelValues();
    const timer = setInterval(() => void controller.refreshChannelValues(), 1500);
    return () => clearInterval(timer);
  });

  function sourceName(channel: Channel): string {
    if (channel.source === 0) return 'virtual';
    return controller.deviceByHandle(channel.source)?.key ?? `device ${channel.source}`;
  }
</script>

<div class="page">
  <section class="panel">
    <h2>Channels</h2>
    {#if rows.length === 0}
      <p class="muted">No channels. Add a device first.</p>
    {:else}
      <table>
        <thead>
          <tr>
            <th>Key</th><th>Source</th><th>Quantity</th>
            <th class="right">Raw</th><th class="right">Calibrated</th>
            <th class="right">Processed</th><th>Quality</th>
            <th>Calibration</th><th></th>
          </tr>
        </thead>
        <tbody>
          {#each rows as channel (channel.handle)}
            <tr
              class:selected={selectedKey === channel.key}
              onclick={() => (selectedKey = channel.key)}>
              <td>
                <strong>{channel.key}</strong>
                <div class="muted small">{channel.name}</div>
              </td>
              <td class="muted">{sourceName(channel)}</td>
              <td class="muted">{channel.quantity}</td>
              <!-- All three stages side by side: this is how you tell "the
                   sensor is dead" from "the calibration is wrong" (§48). -->
              <td class="right numeric muted">
                {formatValue(channel.value?.raw, 1, controller.qualityOf(channel.handle))}
              </td>
              <td class="right numeric muted">
                {formatValue(channel.value?.calibrated, channel.precision,
                             controller.qualityOf(channel.handle))}
              </td>
              <td class="right numeric">
                {formatValue(
                  controller.values[channel.handle] ?? channel.value?.processed,
                  channel.precision,
                  controller.qualityOf(channel.handle))}
                <span class="unit">{channel.unit}</span>
              </td>
              <td class={qualityClass(controller.qualityOf(channel.handle))}>
                {qualityLabel(controller.qualityOf(channel.handle)) || 'good'}
              </td>
              <td>
                {#if controller.activeCalibration(channel.key)}
                  {@const record = controller.activeCalibration(channel.key)!}
                  <span class="cal" title="{record.kind}{record.note ? ` — ${record.note}` : ''}">
                    v{record.version}
                  </span>
                  {#if record.kind !== 'table'}
                    <span class="muted small numeric">
                      ±{record.fit.rms_residual.toPrecision(2)}
                    </span>
                  {/if}
                {:else}
                  <!-- Not a blank cell: "uncalibrated" is a fact about the
                       number in the row next to it. -->
                  <span class="muted small">uncalibrated</span>
                {/if}
              </td>
              <td class="row-actions">
                <button type="button" onclick={(e) => { e.stopPropagation(); calibrate(channel); }}>
                  Calibrate
                </button>
                <button type="button" onclick={(e) => { e.stopPropagation(); editPipeline(channel); }}>
                  Processing
                </button>
              </td>
            </tr>
          {/each}
        </tbody>
      </table>
    {/if}
  </section>

  {#if selected}
    <section class="panel">
      <h2>{selected.key}</h2>
      <dl>
        <dt>Unit</dt><dd>{selected.unit || '—'}</dd>
        <dt>Quantity</dt><dd>{selected.quantity}</dd>
        <dt>Direction</dt><dd>{selected.direction}</dd>
        <dt>Precision</dt><dd>{selected.precision} decimals</dd>
        <dt>Range</dt>
        <dd>
          {#if selected.min < selected.max}
            {selected.min} … {selected.max} {selected.unit}
          {:else}
            not declared
          {/if}
        </dd>
        <dt>Logged</dt><dd>{selected.logged ? 'yes' : 'no'}</dd>
        {#if selected.geometry && selected.geometry.system !== 'none'}
          <dt>Position</dt>
          <dd>
            {selected.geometry.system === 'cylindrical'
              ? `r=${selected.geometry.a} mm, φ=${selected.geometry.b}°, z=${selected.geometry.c} mm`
              : `x=${selected.geometry.a}, y=${selected.geometry.b}, z=${selected.geometry.c} mm`}
            {#if selected.geometry.role}<span class="muted"> — {selected.geometry.role}</span>{/if}
          </dd>
        {/if}
      </dl>
      <p class="note">
        Calibration and the processing chain get their own editor in Milestone 5.
        Until then a chain can be set through <code>PUT /api/v1/processing/{selected.key}</code>.
      </p>
    </section>
  {/if}
</div>

<CalibrationDialog bind:open={calibrating} channel={target}
                   onchanged={() => void controller.refreshChannelValues()} />
<PipelineEditor bind:open={editingPipeline} channel={target}
                oncalibrate={() => { editingPipeline = false; calibrating = true; }} />

<style>
  .page { display: grid; gap: 1rem; }
  .cal { font-size: 0.7rem; color: var(--ok); border: 1px solid var(--ok);
         border-radius: 3px; padding: 0 0.25rem; }
  .small { font-size: 0.72rem; }
  .row-actions { white-space: nowrap; text-align: right; }
  .row-actions button { background: var(--surface-2); border: 1px solid var(--line);
                        color: var(--text); border-radius: 6px; padding: 0.15rem 0.5rem;
                        cursor: pointer; font: inherit; font-size: 0.72rem;
                        margin-left: 0.25rem; }
  .panel { background: var(--surface); border: 1px solid var(--line);
           border-radius: 8px; padding: 0.8rem 0.9rem; display: grid; gap: 0.6rem; }
  h2 { margin: 0; font-size: 0.8rem; text-transform: uppercase;
       letter-spacing: 0.07em; color: var(--muted); font-weight: 600; }
  table { width: 100%; border-collapse: collapse; font-size: 0.85rem; }
  th { text-align: left; font-weight: 500; font-size: 0.68rem; text-transform: uppercase;
       letter-spacing: 0.05em; color: var(--muted); padding-bottom: 0.35rem; }
  th.right, td.right { text-align: right; }
  td { padding: 0.4rem 0.4rem 0.4rem 0; border-top: 1px solid var(--line); }
  tbody tr { cursor: pointer; }
  tbody tr:hover { background: var(--surface-2); }
  tbody tr.selected { background: var(--surface-2); }
  .small { font-size: 0.72rem; }
  .muted { color: var(--muted); }
  .unit { font-size: 0.72rem; color: var(--muted); }
  .note { font-size: 0.75rem; color: var(--muted); margin: 0; }
  dl { display: grid; grid-template-columns: 8rem 1fr; gap: 0.25rem 1rem;
       margin: 0; font-size: 0.85rem; }
  dt { color: var(--muted); font-size: 0.75rem; }
  dd { margin: 0; }
  code { font-family: var(--font-mono); font-size: 0.75rem; }
</style>
