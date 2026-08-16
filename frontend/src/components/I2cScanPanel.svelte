<script lang="ts">
  // ===========================================================================
  //  I2cScanPanel — "what is actually plugged into this bus?"
  //
  //  The single most useful diagnostic in a lab rig: it answers "is it wired
  //  correctly" before anything has been configured.  Candidates are presented
  //  as candidates — 0x76 is BMP280 *and* BME280 *and* several unrelated parts,
  //  and only the driver's chip-ID check can settle it.
  // ===========================================================================
  import { api, type I2cScanResult } from '../lib/api';
  import { describe } from '../lib/state.svelte';

  let {
    bus = 0,
    onpick = (_module: string, _address: string) => {},
  }: {
    bus?: number;
    onpick?: (module: string, address: string) => void;
  } = $props();

  let result = $state<I2cScanResult | null>(null);
  let busy = $state(false);
  let error = $state('');

  // A result belongs to the bus it came from; switching buses must not leave
  // the previous bus's devices on screen under the new bus's heading.
  $effect(() => {
    bus;
    result = null;
    error = '';
  });

  async function scan() {
    busy = true;
    error = '';
    try {
      result = await api.scanI2c(bus);
    } catch (e) {
      error = describe(e);
      result = null;
    } finally {
      busy = false;
    }
  }
</script>

<div class="scan">
  <div class="head">
    <strong>I²C bus {bus}</strong>
    <button type="button" onclick={scan} disabled={busy}>
      {busy ? 'Scanning…' : 'Scan bus'}
    </button>
  </div>

  {#if error}
    <p class="error">{error}</p>
  {/if}

  {#if result}
    {#if result.found.length === 0}
      <p class="empty">
        No device answered. Check SDA/SCL wiring, pull-ups and the sensor's supply.
      </p>
    {:else}
      <table>
        <thead>
          <tr><th>Address</th><th>Probably</th><th>Status</th><th></th></tr>
        </thead>
        <tbody>
          {#each result.found as device (device.address)}
            <tr>
              <td class="numeric">{device.address}</td>
              <td>
                {#if device.candidates.length === 0}
                  <span class="muted">unknown device</span>
                {:else}
                  {#each device.candidates as candidate, i (candidate.module)}
                    {#if i > 0}<span class="muted"> / </span>{/if}
                    <span class:likely={candidate.confidence === 'likely'}>
                      {candidate.label}
                    </span>
                  {/each}
                {/if}
              </td>
              <td>
                {#if device.claimed_by}
                  <span class="muted">used by {device.claimed_by}</span>
                {:else}
                  <span class="free">free</span>
                {/if}
              </td>
              <td>
                {#if !device.claimed_by && device.candidates.length > 0}
                  <button
                    type="button"
                    class="link"
                    onclick={() => onpick(device.candidates[0]!.module, device.address)}>
                    Add {device.candidates[0]!.module}
                  </button>
                {/if}
              </td>
            </tr>
          {/each}
        </tbody>
      </table>
      <p class="note">
        A matching address is a hint, not an identification. The driver confirms
        the part by reading its chip ID when the device starts.
      </p>
    {/if}
  {/if}
</div>

<style>
  .scan { display: grid; gap: 0.6rem; }
  .head { display: flex; align-items: center; justify-content: space-between; gap: 1rem; }
  table { width: 100%; border-collapse: collapse; font-size: 0.85rem; }
  th { text-align: left; font-weight: 500; font-size: 0.7rem; text-transform: uppercase;
       letter-spacing: 0.05em; color: var(--muted); padding-bottom: 0.3rem; }
  td { padding: 0.3rem 0; border-top: 1px solid var(--line); }
  .likely { color: var(--text); }
  .muted { color: var(--muted); }
  .free { color: var(--ok); }
  .empty, .note { font-size: 0.78rem; color: var(--muted); margin: 0; }
  .error { color: var(--danger); font-size: 0.8rem; }
  .link { background: none; border: 0; color: var(--accent); cursor: pointer; padding: 0; font: inherit; }
</style>
