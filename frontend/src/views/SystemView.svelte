<script lang="ts">
  import { api } from '../lib/api';
  import { formatDuration } from '../lib/format';
  import { controller, describe } from '../lib/state.svelte';

  let busy = $state('');
  let error = $state('');
  let message = $state('');

  const system = $derived(controller.system ?? {});

  async function exportConfig() {
    busy = 'export';
    error = '';
    message = '';
    try {
      const document_ = await api.exportConfig();
      const blob = new Blob([JSON.stringify(document_, null, 2)],
                            { type: 'application/json' });
      const url = URL.createObjectURL(blob);
      const link = Object.assign(window.document.createElement('a'), {
        href: url,
        download: `lab-controller-config-${new Date().toISOString().slice(0, 10)}.json`,
      });
      // The anchor must be in the document to be reliably actionable, and the
      // object URL must outlive the click: revoking it in the same tick can
      // cancel the download with no error anywhere.  A silent failure here is
      // the worst kind — this is the button people press before an upgrade.
      window.document.body.appendChild(link);
      link.click();
      link.remove();
      setTimeout(() => URL.revokeObjectURL(url), 30_000);
      message = 'Configuration exported.';
    } catch (e) {
      error = describe(e);
    } finally {
      busy = '';
    }
  }

  async function importConfig(event: Event) {
    const input = event.target as HTMLInputElement;
    const file = input.files?.[0];
    if (!file) return;
    busy = 'import';
    error = '';
    message = '';
    try {
      const parsed = JSON.parse(await file.text());
      // Confirmed by the password, not by the session: an import replaces every
      // section — interlocks included — on a rig that may be running
      // (ADR-0020).  Asked for here rather than discovered as a 403.
      let confirmation: string | undefined;
      if (controller.auth.configured) {
        confirmation = prompt(
          'Importing replaces the whole configuration, including safety limits.\n' +
          'Confirm with your password:') ?? undefined;
        if (!confirmation) {
          busy = '';
          input.value = '';
          return;
        }
      }
      const result = await api.importConfig(parsed, confirmation);
      // Partial success is the normal case when moving a configuration between
      // boards with different pinouts — report it rather than pretending.
      message = `${result.sections_written} sections written, ` +
                `${result.devices_started} devices started` +
                (result.devices_failed ? `, ${result.devices_failed} failed` : '') +
                (result.backup_saved ? ' · the previous configuration was kept' : '');
      await controller.refresh();
    } catch (e) {
      error = describe(e);
    } finally {
      busy = '';
      input.value = '';
    }
  }

  let currentPassword = $state('');
  let newPassword = $state('');

  async function changePassword() {
    busy = 'password';
    error = '';
    message = '';
    try {
      await api.setPassword(newPassword,
                            controller.auth.configured ? currentPassword : undefined);
      currentPassword = '';
      newPassword = '';
      message = 'Password changed. Every session ended, including this one.';
      await controller.refreshAuth();
    } catch (e) {
      error = describe(e);
    } finally {
      busy = '';
    }
  }

  async function reboot() {
    if (!confirm('Reboot the controller? Any running acquisition stops.')) return;
    busy = 'reboot';
    error = '';
    message = '';
    try {
      await api.reboot();
      message = 'Rebooting — the page will reconnect on its own.';
    } catch (e) {
      error = describe(e);
    } finally {
      busy = '';
    }
  }
</script>

<div class="page">
  <section class="panel">
    <h2>Controller</h2>
    <dl>
      <dt>Firmware</dt><dd class="numeric">{system.firmware ?? '—'}</dd>
      <dt>Chip</dt><dd>{system.chip ?? '—'}</dd>
      <dt>Boot mode</dt>
      <dd>
        {system.boot_mode ?? '—'}
        {#if system.boot_mode === 'SAFE'}
          <span class="warn"> — devices were not started; fix the configuration and reboot</span>
        {/if}
      </dd>
      <dt>Uptime</dt><dd class="numeric">{formatDuration(system.uptime_ms)}</dd>
      <dt>Clock</dt>
      <dd>
        {#if system.time_synchronised}
          synchronised
        {:else}
          <!-- Honest: logs written now carry monotonic time only. -->
          <span class="warn">not synchronised — logs will carry relative time</span>
        {/if}
      </dd>
      <dt>Config revision</dt><dd class="numeric">{system.config_revision ?? '—'}</dd>
      <dt>Last boot</dt>
      <dd>
        <span class="numeric">{system.boot?.devices_started ?? 0}</span> devices started
        {#if (system.boot?.devices_failed ?? 0) > 0}
          <span class="warn">
            , <span class="numeric">{system.boot.devices_failed}</span> failed
            {#if system.boot.first_failure}
              — <span class="numeric">{system.boot.first_failure.device}</span>
              {#if system.boot.first_failure.field}({system.boot.first_failure.field}){/if}:
              {system.boot.first_failure.detail || system.boot.first_failure.code}
            {/if}
          </span>
        {/if}
        {#if !(system.boot?.storage_mounted ?? true)}
          <span class="warn"> — storage did not mount, nothing was loaded</span>
        {/if}
      </dd>
    </dl>
  </section>

  <section class="panel">
    <h2>Network</h2>
    <dl>
      <dt>Mode</dt><dd>{system.network_mode ?? '—'}</dd>
      <dt>Address</dt><dd class="numeric">{system.ip ?? '—'}</dd>
      <dt>Hostname</dt><dd class="numeric">{system.hostname ?? '—'}</dd>
      <dt>Link</dt>
      <dd class:ok={controller.connected} class:bad={!controller.connected}>
        {controller.connected ? 'telemetry connected' : 'telemetry offline'}
      </dd>
    </dl>
    <p class="note">
      If the configured Wi-Fi cannot be joined the controller opens its own
      access point (<span class="numeric">LAB-CONTROLLER</span>, 192.168.4.1) and
      keeps retrying in the background. It never becomes unreachable.
    </p>
  </section>

  <section class="panel">
    <h2>Configuration</h2>
    <div class="row">
      <button type="button" onclick={exportConfig} disabled={busy !== ''}>
        Export configuration
      </button>
      <label class="file">
        <input type="file" accept="application/json" onchange={importConfig} />
        <span>Import configuration…</span>
      </label>
      <button type="button" class="danger" onclick={reboot} disabled={busy !== ''}>
        Reboot
      </button>
    </div>
    {#if message}<p class="note">{message}</p>{/if}
    {#if error}<p class="error">{error}</p>{/if}
    <p class="note">
      An export contains devices, channels, processing, calibrations and rules —
      everything needed to rebuild this rig on another board. Wi-Fi credentials
      and passwords are deliberately excluded: the credential is not a
      configuration section, so the export cannot reach it even by mistake.
    </p>
    <p class="note">
      The configuration that an import replaces is kept — one file, overwritten
      by each import. It is an undo for the most destructive thing this
      interface can do, not a version history.
    </p>
  </section>

  <section class="panel">
    <h2>Access</h2>
    {#if !controller.auth.configured}
      <p class="note">
        No password is set. Anyone on this network can change this instrument.
      </p>
    {:else}
      <dl>
        <dt>sessions</dt><dd>{controller.auth.sessions} open</dd>
        <dt>this browser</dt>
        <dd>{controller.auth.signed_in ? 'signed in' : 'signed out'}</dd>
      </dl>
    {/if}
    <div class="row">
      <input type="password" placeholder="current password"
             bind:value={currentPassword} autocomplete="current-password" />
      <input type="password" placeholder="new password (8+)"
             bind:value={newPassword} autocomplete="new-password" />
      <button type="button" disabled={busy !== '' || newPassword.length < 8}
              onclick={changePassword}>Change password</button>
    </div>
    <p class="note">
      Changing the password ends every session, including this one. The
      emergency stop and stopping a run never require signing in — a person
      reaching for the stop button is not going to type a password first.
    </p>
  </section>
</div>

<style>
  .page { display: grid; gap: 1rem;
          grid-template-columns: repeat(auto-fit, minmax(320px, 1fr)); }
  .panel { background: var(--surface); border: 1px solid var(--line);
           border-radius: 8px; padding: 0.8rem 0.9rem; display: grid;
           gap: 0.6rem; align-content: start; }
  h2 { margin: 0; font-size: 0.8rem; text-transform: uppercase;
       letter-spacing: 0.07em; color: var(--muted); font-weight: 600; }
  dl { display: grid; grid-template-columns: 9rem 1fr; gap: 0.3rem 1rem;
       margin: 0; font-size: 0.85rem; }
  dt { color: var(--muted); font-size: 0.75rem; }
  dd { margin: 0; }
  .row { display: flex; flex-wrap: wrap; gap: 0.5rem; align-items: center; }
  .note { font-size: 0.75rem; color: var(--muted); margin: 0; }
  .error { font-size: 0.8rem; color: var(--danger); margin: 0; }
  .warn { color: var(--warn); }
  .ok { color: var(--ok); }
  .bad { color: var(--warn); }
  button { background: var(--surface-2); border: 1px solid var(--line); color: var(--text);
           border-radius: 6px; padding: 0.3rem 0.7rem; cursor: pointer; font-size: 0.8rem; }
  button.danger:hover { border-color: var(--danger); color: var(--danger); }
  button:disabled { opacity: 0.5; cursor: default; }
  .file input { display: none; }
  .file span { display: inline-block; background: var(--surface-2);
               border: 1px solid var(--line); border-radius: 6px;
               padding: 0.3rem 0.7rem; cursor: pointer; font-size: 0.8rem; }
</style>
