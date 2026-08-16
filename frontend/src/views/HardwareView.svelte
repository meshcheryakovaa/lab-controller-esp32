<script lang="ts">
  import AddDeviceDialog from '../components/AddDeviceDialog.svelte';
  import I2cScanPanel from '../components/I2cScanPanel.svelte';
  import StatusPill from '../components/StatusPill.svelte';
  import { api } from '../lib/api';
  import { formatInterval } from '../lib/format';
  import { controller, describe } from '../lib/state.svelte';

  let adding = $state(false);
  let addPreset = $state<{ module: string; address?: string } | null>(null);
  // The scanner can look at any configured bus, not only bus 0.
  let scanBus = $state(0);
  let busy = $state('');
  let message = $state('');
  let error = $state('');

  async function act(key: string, action: string) {
    busy = key + action;
    error = '';
    message = '';
    try {
      const result = await api.deviceAction(key, action);
      if (action === 'self-test') {
        // A failed self-test is a failure, not a note: rendering it in the same
        // muted style as a pass is how a broken sensor gets overlooked.
        const text = result.passed
          ? `${key}: self-test passed`
          : `${key}: ${(result.error as any)?.detail ?? 'self-test failed'}`;
        if (result.passed) message = text;
        else error = text;
      }
      await controller.refresh();
    } catch (e) {
      error = describe(e);
    } finally {
      busy = '';
    }
  }

  async function remove(key: string) {
    // Deleting a device also deletes its channels; anything pointing at them
    // needs to know, so say it plainly rather than after the fact.
    if (!confirm(`Delete "${key}"? Its channels and their history will be removed.`)) return;
    busy = key + 'delete';
    error = '';
    try {
      await api.deleteDevice(key);
      await controller.refresh();
    } catch (e) {
      error = describe(e);
    } finally {
      busy = '';
    }
  }

  const claimedPins = $derived(
    (controller.gpio?.pins ?? []).filter((p) => p.owner !== undefined),
  );
</script>

<div class="page">
  <section class="panel">
    <header>
      <h2>Devices</h2>
      <button type="button" class="primary" onclick={() => (adding = true)}>Add device</button>
    </header>

    {#if error}<p class="error">{error}</p>{/if}
    {#if message}<p class="note">{message}</p>{/if}

    {#if controller.devices.length === 0}
      <p class="muted">
        Nothing configured yet. The rig is described entirely by the
        configuration on the board — adding a device here writes it there.
      </p>
    {:else}
      <table>
        <thead>
          <tr>
            <th>Key</th><th>Module</th><th>State</th><th>Rate</th>
            <th>Channels</th><th></th>
          </tr>
        </thead>
        <tbody>
          {#each controller.devices as device (device.handle)}
            <tr>
              <td>
                <strong>{device.key}</strong>
                <div class="muted small">{device.name}</div>
              </td>
              <td class="muted">{device.module}</td>
              <td>
                <StatusPill state={device.state} />
                {#if device.error}
                  <!-- §40: an error must say what is wrong, not just that
                       something is. -->
                  <div class="error small">{device.error.detail || device.error.code}</div>
                {/if}
              </td>
              <td class="numeric">{formatInterval(device.sample_interval_us)}</td>
              <td>
                {#each device.channels ?? [] as channel (channel.handle)}
                  <span class="chan" title="{channel.unit || 'no unit'} · {channel.stages} processing stage(s)">
                    {channel.key}
                  </span>
                {/each}
              </td>
              <td class="actions">
                <button type="button" onclick={() => act(device.key, 'self-test')}
                        disabled={busy !== ''}>Test</button>
                {#if device.state === 'DISABLED'}
                  <button type="button" onclick={() => act(device.key, 'enable')}
                          disabled={busy !== ''}>Enable</button>
                {:else}
                  <button type="button" onclick={() => act(device.key, 'disable')}
                          disabled={busy !== ''}>Disable</button>
                {/if}
                <button type="button" class="danger" onclick={() => remove(device.key)}
                        disabled={busy !== ''}>Delete</button>
              </td>
            </tr>
          {/each}
        </tbody>
      </table>
    {/if}
  </section>

  <section class="panel">
    <header>
      <h2>Buses</h2>
      {#if (controller.gpio?.i2c_buses ?? 1) > 1}
        <label class="bus-pick">
          bus
          <select bind:value={scanBus}>
            {#each Array.from({ length: controller.gpio?.i2c_buses ?? 1 }, (_, i) => i) as index (index)}
              <option value={index}>I²C{index}</option>
            {/each}
          </select>
        </label>
      {/if}
    </header>
    <I2cScanPanel bus={scanBus} onpick={(module, address) => {
      addPreset = { module, address };
      adding = true;
    }} />
  </section>

  <section class="panel">
    <h2>Pin usage</h2>
    {#if claimedPins.length === 0}
      <p class="muted">No pins claimed.</p>
    {:else}
      <ul class="pins">
        {#each claimedPins as pin (pin.pin)}
          <li>
            <span class="numeric">GPIO{pin.pin}</span>
            <span class="muted">{pin.owner}</span>
          </li>
        {/each}
      </ul>
    {/if}
    <p class="note">
      Ownership is tracked by the firmware, not by this page. A pin that is
      taken cannot be selected anywhere else, and the reason is always named.
    </p>
  </section>
</div>

<AddDeviceDialog bind:open={adding} bind:preset={addPreset} />

<style>
  .page { display: grid; gap: 1rem; }
  .panel { background: var(--surface); border: 1px solid var(--line);
           border-radius: 8px; padding: 0.8rem 0.9rem; display: grid; gap: 0.7rem; }
  .panel header { display: flex; align-items: center; justify-content: space-between; }
  h2 { margin: 0; font-size: 0.8rem; text-transform: uppercase;
       letter-spacing: 0.07em; color: var(--muted); font-weight: 600; }
  table { width: 100%; border-collapse: collapse; font-size: 0.85rem; }
  th { text-align: left; font-weight: 500; font-size: 0.68rem; text-transform: uppercase;
       letter-spacing: 0.05em; color: var(--muted); padding-bottom: 0.35rem; }
  td { padding: 0.45rem 0.4rem 0.45rem 0; border-top: 1px solid var(--line);
       vertical-align: top; }
  .small { font-size: 0.72rem; }
  .muted { color: var(--muted); }
  .error { color: var(--danger); }
  .note { font-size: 0.75rem; color: var(--muted); margin: 0; }
  .chan { display: inline-block; font-size: 0.72rem; color: var(--muted);
          border: 1px solid var(--line); border-radius: 3px; padding: 0 0.3rem;
          margin: 0 0.2rem 0.2rem 0; }
  .actions { white-space: nowrap; text-align: right; }
  .pins { list-style: none; margin: 0; padding: 0; display: grid;
          grid-template-columns: repeat(auto-fill, minmax(180px, 1fr)); gap: 0.25rem; }
  .pins li { display: flex; gap: 0.5rem; font-size: 0.8rem; }
  .bus-pick { font-size: 0.75rem; color: var(--muted); display: flex;
              align-items: center; gap: 0.35rem; }
  button { background: var(--surface-2); border: 1px solid var(--line); color: var(--text);
           border-radius: 6px; padding: 0.2rem 0.6rem; cursor: pointer; font-size: 0.78rem;
           margin-left: 0.25rem; }
  button.primary { background: var(--accent); border-color: var(--accent);
                   color: #05121f; font-weight: 600; }
  button.danger:hover { border-color: var(--danger); color: var(--danger); }
  button:disabled { opacity: 0.5; cursor: default; }
</style>
