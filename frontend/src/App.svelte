<script lang="ts">
  import { onMount } from 'svelte';
  import { api } from './lib/api';
  import { controller, describe } from './lib/state.svelte';
  import SignInBar from './components/SignInBar.svelte';
  import ChannelsView from './views/ChannelsView.svelte';
  import ControlView from './views/ControlView.svelte';
  import ExperimentsView from './views/ExperimentsView.svelte';
  import LogsView from './views/LogsView.svelte';
  import DashboardView from './views/DashboardView.svelte';
  import DiagnosticsView from './views/DiagnosticsView.svelte';
  import HardwareView from './views/HardwareView.svelte';
  import CloudView from './views/CloudView.svelte';
  import NetworkView from './views/NetworkView.svelte';
  import SystemView from './views/SystemView.svelte';
  import LocalDataView from './views/LocalDataView.svelte';
  import LocalRecordingStatus from './components/LocalRecordingStatus.svelte';
  import { recorder } from './lib/client-recorder.svelte';

  const sections = [
    { id: 'dashboard', label: 'Dashboard' },
    { id: 'hardware', label: 'Hardware' },
    { id: 'channels', label: 'Channels' },
    { id: 'control', label: 'Control' },
    { id: 'experiments', label: 'Experiments' },
    { id: 'logs', label: 'Logs' },
    // M14.  Deliberately its own page and not a tab of Logs: those datasets are
    // on the controller and survive a closed browser; these are on this device
    // and do not.  Filing them together would blur exactly that difference.
    { id: 'local', label: 'Local data' },
    // M16.  Next to System rather than inside it: connectivity is the one
    // setting that can put the instrument out of reach, and burying it a level
    // down is not where somebody looks when they cannot find the device.
    { id: 'network', label: 'Network' },
    // M17.  Next to Network, because that is what it depends on: the uploader
    // will not attempt anything until the controller is on a real router.
    { id: 'cloud', label: 'Cloud' },
    { id: 'system', label: 'System' },
    { id: 'diagnostics', label: 'Diagnostics' },
  ] as const;

  type SectionId = (typeof sections)[number]['id'];

  let active = $state<SectionId>('dashboard');
  let booting = $state(true);
  let bootError = $state('');

  onMount(() => {
    const stop = controller.start();
    // pagehide may be the last code that runs, and may not run at all.  Used
    // for a best-effort flush only; nothing is built on it (§14).
    const onHide = () => recorder.flushBeforeUnload();
    window.addEventListener('pagehide', onHide);
    void (async () => {
      try {
        await controller.loadStatic();
        await controller.refresh();
      } catch (e) {
        bootError = describe(e);
      } finally {
        booting = false;
      }
    })();
    return () => {
      window.removeEventListener('pagehide', onHide);
      stop();
    };
  });

  const errorDevices = $derived(
    controller.devices.filter((d) => d.state === 'ERROR' || d.state === 'WARNING'),
  );

  // A device that is in devices.json but refused to start has no record, no
  // handle and no channel — it is not in `controller.devices` at all.  Without
  // this banner it disappears in silence, and "I never added it" looks exactly
  // like "it failed at boot" (§46).
  const boot = $derived(controller.system?.boot ?? null);
  const bootFailures = $derived(Number(boot?.devices_failed ?? 0));

  // The master stop lives in the frame, not on a page.  An operator reaching
  // for it is not going to navigate first (§30).
  let stopping = $state(false);
  async function toggleStop() {
    stopping = true;
    try {
      if (controller.outputsTripped) {
        if (!confirm('Allow outputs to be commanded again? Nothing switches back on by itself.')) return;
        await api.clearOutputTrip();
      } else {
        await api.tripOutputs();
      }
      await controller.refresh();
    } catch (e) {
      controller.loadError = describe(e);
    } finally {
      stopping = false;
    }
  }
</script>

<div class="shell">
  <aside>
    <div class="brand">
      <strong>LAB CONTROLLER</strong>
      <span class="link" class:on={controller.connected}>
        {controller.connected ? 'LIVE' : 'OFFLINE'}
      </span>
    </div>
    <nav>
      {#each sections as section (section.id)}
        <button class:active={active === section.id} onclick={() => (active = section.id)}>
          {section.label}
          {#if section.id === 'hardware' && errorDevices.length > 0}
            <span class="badge">{errorDevices.length}</span>
          {/if}
          {#if section.id === 'experiments' && controller.runningExperiment}
            <!-- A run in progress is the other thing worth knowing from any
                 screen: the rig is being driven by something that is not you. -->
            <span class="badge run">●</span>
          {/if}
          {#if section.id === 'logs' && controller.logging?.recording}
            <span class="badge run">●</span>
          {/if}
          {#if section.id === 'logs' && controller.logging?.last_truncated
               && !controller.logging?.recording}
            <!-- A dataset that stopped early is worth seeing from anywhere:
                 somebody is about to walk off with a file that is missing its
                 last hours. -->
            <span class="badge">!</span>
          {/if}
          {#if section.id === 'control' && controller.latchedLimits > 0}
            <!-- A latched interlock is the one thing on this page that means
                 the rig stopped itself.  It is visible from every screen. -->
            <span class="badge">{controller.latchedLimits}</span>
          {/if}
        </button>
      {/each}
    </nav>
    <div class="foot">
      <span class="muted">{controller.system?.firmware ?? ''}</span>
    </div>
  </aside>

  <main>
    <!-- M14: a running local recording, or one that stopped because the device
         filled up, has to be visible from every page.  A recording that died an
         hour ago while the operator was on the Hardware page is the failure this
         prevents. -->
    <LocalRecordingStatus onopen={() => (active = 'local')} />
    <header class="topbar">
      <h1>{sections.find((s) => s.id === active)?.label}</h1>
      {#if controller.loading}<span class="muted">refreshing…</span>{/if}
      <SignInBar />
      {#if controller.outputCount > 0}
        <!-- Always last, always reachable, never behind a sign-in: the stop is
             the one control that must work for whoever is standing there. -->
        <button type="button" class="stop" class:tripped={controller.outputsTripped}
                disabled={stopping} onclick={toggleStop}>
          {controller.outputsTripped ? 'Outputs stopped — allow' : 'Stop all outputs'}
        </button>
      {/if}
    </header>

    {#if controller.alerts.length > 0}
      <div class="alerts">
        {#each controller.alerts.slice(0, 3) as alert (alert.id)}
          <div class="alert" class:critical={alert.severity >= 3}>
            <span class="code numeric">{alert.code}</span>
            <span>{alert.message}</span>
            <button onclick={() => controller.dismissAlert(alert.id)} aria-label="Dismiss">×</button>
          </div>
        {/each}
      </div>
    {/if}

    {#if controller.outputsTripped}
      <!-- Not a toast and not dismissible: while this is true nothing can be
           commanded, and that fact has to be on screen. -->
      <div class="tripped-banner">
        <strong>All outputs are held at their safe values.</strong>
        <span>{controller.tripReason || 'stopped by the safety layer'}</span>
        <span class="muted">
          Clearing this permits commands again; it does not switch anything back on.
        </span>
      </div>
    {/if}

    {#if controller.loadError && !booting}
      <!-- A refresh or a diagnostics poll that failed used to be recorded and
           never shown: the page simply kept its last snapshot, which reads as
           "everything is fine" (§46). -->
      <div class="alerts">
        <div class="alert">
          <span class="code numeric">NO ANSWER</span>
          <span>{controller.loadError}</span>
          <button onclick={() => (controller.loadError = null)} aria-label="Dismiss">×</button>
        </div>
      </div>
    {/if}

    {#if bootFailures > 0 && !booting}
      <div class="boot-warning">
        <strong>
          {bootFailures} configured {bootFailures === 1 ? 'device' : 'devices'} did not start.
        </strong>
        {#if boot?.first_failure}
          <span>
            <code>{boot.first_failure.device}</code>
            {#if boot.first_failure.field}· field <code>{boot.first_failure.field}</code>{/if}
            · {boot.first_failure.detail || boot.first_failure.code}
          </span>
        {/if}
        <span class="muted">
          They are still in the stored configuration and will be retried at the
          next boot. The rest of the rig kept running.
        </span>
      </div>
    {/if}

    {#if bootError}
      <div class="fatal">
        <strong>Cannot reach the controller.</strong>
        <p>{bootError}</p>
        <p class="muted">
          The board keeps acquiring, logging and regulating without a browser —
          this page is only a window. Check the network and reload.
        </p>
      </div>
    {:else if booting}
      <p class="muted">Loading…</p>
    {:else if active === 'dashboard'}
      <DashboardView />
    {:else if active === 'hardware'}
      <HardwareView />
    {:else if active === 'channels'}
      <ChannelsView />
    {:else if active === 'control'}
      <ControlView />
    {:else if active === 'experiments'}
      <ExperimentsView />
    {:else if active === 'logs'}
      <LogsView />
    {:else if active === 'local'}
      <LocalDataView />
    {:else if active === 'network'}
      <NetworkView />
    {:else if active === 'cloud'}
      <CloudView />
    {:else if active === 'system'}
      <SystemView />
    {:else}
      <DiagnosticsView />
    {/if}
  </main>
</div>

<style>
  .shell { display: grid; grid-template-columns: 210px 1fr; min-height: 100vh; }
  aside { border-right: 1px solid var(--line); padding: 1rem 0.75rem;
          display: flex; flex-direction: column; gap: 1rem; }
  .brand { display: flex; justify-content: space-between; align-items: baseline;
           font-size: 0.78rem; letter-spacing: 0.08em; }
  .link { font-size: 0.65rem; color: var(--muted); }
  .link.on { color: var(--ok); }
  nav { display: grid; gap: 0.15rem; }
  nav button { display: flex; align-items: center; justify-content: space-between;
               text-align: left; background: none; border: 0; color: inherit;
               padding: 0.4rem 0.6rem; border-radius: 6px; font: inherit;
               cursor: pointer; font-size: 0.88rem; }
  nav button:hover { background: var(--surface-2); }
  nav button.active { background: var(--surface-2); color: var(--accent); }
  .badge { background: var(--warn); color: #10131a; border-radius: 999px;
           font-size: 0.62rem; padding: 0 0.35rem; font-weight: 700; }
  .badge.run { background: transparent; color: var(--ok); font-size: 0.7rem;
               padding: 0; }
  .stop { margin-left: auto; background: var(--surface-2); color: var(--danger);
          border: 1px solid var(--danger); border-radius: 6px;
          padding: 0.25rem 0.7rem; cursor: pointer; font: inherit;
          font-size: 0.78rem; font-weight: 600; }
  .stop.tripped { background: var(--danger); color: #10131a; }
  .stop:disabled { opacity: 0.6; cursor: default; }
  .tripped-banner { display: grid; gap: 0.15rem; margin-bottom: 0.9rem;
                    padding: 0.6rem 0.8rem; border-radius: 7px; font-size: 0.82rem;
                    background: color-mix(in srgb, var(--danger) 14%, var(--surface));
                    border: 1px solid var(--danger); }
  .tripped-banner .muted { font-size: 0.74rem; }
  .boot-warning { display: grid; gap: 0.2rem; margin-bottom: 0.9rem;
                  padding: 0.6rem 0.8rem; border-radius: 7px; font-size: 0.82rem;
                  background: color-mix(in srgb, var(--warn) 12%, var(--surface));
                  border: 1px solid var(--warn); }
  .boot-warning code { font-family: ui-monospace, SFMono-Regular, monospace; font-size: 0.78rem; }
  .boot-warning .muted { font-size: 0.75rem; }
  .foot { margin-top: auto; font-size: 0.7rem; }
  main { padding: 1.1rem 1.5rem 2rem; min-width: 0; }
  .topbar { display: flex; align-items: baseline; gap: 1rem; margin-bottom: 0.9rem; }
  h1 { font-size: 1.05rem; font-weight: 600; letter-spacing: 0.01em; margin: 0; }
  .muted { color: var(--muted); font-size: 0.8rem; }
  .alerts { display: grid; gap: 0.3rem; margin-bottom: 0.9rem; }
  .alert { display: flex; align-items: center; gap: 0.6rem; font-size: 0.8rem;
           background: var(--surface); border: 1px solid var(--warn);
           border-radius: 6px; padding: 0.35rem 0.6rem; }
  .alert.critical { border-color: var(--danger); }
  .alert .code { font-size: 0.7rem; color: var(--muted); }
  .alert button { margin-left: auto; background: none; border: 0; color: var(--muted);
                  cursor: pointer; font-size: 1rem; line-height: 1; }
  .fatal { border: 1px solid var(--danger); border-radius: 8px; padding: 1rem;
           max-width: 40rem; }
  .fatal p { margin: 0.4rem 0 0; font-size: 0.85rem; }

  @media (max-width: 760px) {
    .shell { grid-template-columns: 1fr; }
    aside { border-right: 0; border-bottom: 1px solid var(--line); }
    nav { grid-auto-flow: column; overflow-x: auto; }
    .foot { display: none; }
    main { padding: 1rem; }
  }
</style>
