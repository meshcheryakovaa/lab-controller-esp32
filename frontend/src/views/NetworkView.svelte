<script lang="ts">
  // ==========================================================================
  //  NetworkView — joining the house network without losing the instrument.
  //
  //  THE ONE RULE (ADR-0022): changing the network must never make the
  //  controller unreachable.  Three things in here exist only because of it:
  //
  //   * Nothing on this page ever navigates for you.  After a successful join
  //     the new addresses are offered as LINKS.  An automatic redirect looks
  //     helpful and is the exact move that loses the screen when .local does
  //     not resolve — and the operator is usually still on the fallback AP,
  //     where the new address is not reachable at all yet.
  //   * The fallback access point is always on screen, with its address, so
  //     the way back is visible before it is needed rather than after.
  //   * The password field is write-only.  The controller never sends one back
  //     (there is no field in the response for it), so this box starts empty
  //     every time rather than showing dots that imply something is there.
  // ==========================================================================
  import { onMount } from 'svelte';
  import { api } from '../lib/api';
  import { describe } from '../lib/state.svelte';
  import type { NetworkCandidate, NetworkStatus } from '../lib/types';

  let status = $state<NetworkStatus | null>(null);
  let networks = $state<NetworkCandidate[]>([]);
  let scanning = $state(false);
  let busy = $state('');
  let error = $state('');
  let message = $state('');

  let ssid = $state('');
  let password = $state('');
  let showPassword = $state(false);
  let hidden = $state(false);
  let hostname = $state('');
  let confirmingForget = $state(false);

  // How often the status is re-read.  One second while an attempt is in
  // flight, because that is what the operator is watching; five otherwise, so
  // an idle page is not a load on a device that is also running an experiment.
  const ACTIVE_POLL_MS = 1000;
  const IDLE_POLL_MS = 5000;

  let timer: ReturnType<typeof setTimeout> | null = null;

  const connecting = $derived(
    status?.pending === true || status?.state === 'STA_CONNECTING');

  const modeLabel = $derived.by(() => {
    if (!status) return '—';
    if (status.station.connected && status.access_point.active) return 'AP + STA';
    if (status.station.connected) return 'STA';
    if (status.access_point.active) return 'AP';
    return '—';
  });

  /** Signal quality in words.  A number in dBm is not something most people
   *  reading a lab bench display can act on; "weak" is. */
  function signalLabel(rssi: number): string {
    if (rssi === 0) return '—';
    if (rssi >= -55) return `Excellent (${rssi} dBm)`;
    if (rssi >= -67) return `Good (${rssi} dBm)`;
    if (rssi >= -75) return `Fair (${rssi} dBm)`;
    return `Weak (${rssi} dBm)`;
  }

  function signalClass(rssi: number): string {
    if (rssi === 0) return '';
    if (rssi >= -55) return 'good';
    if (rssi >= -67) return 'good';
    if (rssi >= -75) return 'fair';
    return 'weak';
  }

  async function refresh() {
    try {
      const next = await api.network();
      // A join that has finished is worth saying out loud once, because the
      // page the operator is looking at may be about to stop being reachable.
      if (status?.pending && !next.pending) {
        // Said once, either way.  A success banner left standing above a card
        // that now reports a failure is worse than no banner at all — and the
        // banner is the part an operator reads first.
        message = next.station.connected ? `Connected to ${next.ssid}.` : '';
      }
      if (next.last_error && !next.station.connected) message = '';
      status = next;
      error = '';
    } catch (e) {
      // A failed poll during a network change is expected, not alarming: the
      // interface the browser is using may be the one being reconfigured.
      if (!connecting) error = describe(e);
    }
    schedule();
  }

  function schedule() {
    if (timer !== null) clearTimeout(timer);
    timer = setTimeout(refresh, connecting ? ACTIVE_POLL_MS : IDLE_POLL_MS);
  }

  onMount(() => {
    void refresh();
    return () => { if (timer !== null) clearTimeout(timer); };
  });

  async function scan() {
    scanning = true;
    error = '';
    try {
      await api.startNetworkScan();
      // The scan is asynchronous on the device; poll until it settles rather
      // than blocking a request for the seconds a scan takes.
      for (let attempt = 0; attempt < 15; ++attempt) {
        await new Promise((resolve) => setTimeout(resolve, 700));
        const result = await api.networkScan();
        if (result.state === 'COMPLETE') {
          networks = result.networks;
          return;
        }
        if (result.state === 'FAILED') {
          error = 'The scan did not complete. Try again.';
          return;
        }
      }
      error = 'The scan is taking longer than expected.';
    } catch (e) {
      error = describe(e);
    } finally {
      scanning = false;
    }
  }

  function choose(candidate: NetworkCandidate) {
    ssid = candidate.ssid;
    hidden = false;
    password = '';
  }

  async function connect() {
    busy = 'connect';
    error = '';
    message = '';
    try {
      await api.connectNetwork(ssid.trim(), password);
      // Cleared immediately: it has been sent, and there is no reason for it to
      // stay in a DOM node on a shared bench machine.
      password = '';
      await refresh();
    } catch (e) {
      error = describe(e);
    } finally {
      busy = '';
    }
  }

  async function forget() {
    busy = 'forget';
    error = '';
    message = '';
    try {
      const result = await api.forgetNetwork();
      message = `Home network forgotten. The controller is now at `
              + `http://${result.ip} on ${result.ssid}.`;
      confirmingForget = false;
      await refresh();
    } catch (e) {
      error = describe(e);
    } finally {
      busy = '';
    }
  }

  async function renameHost() {
    busy = 'hostname';
    error = '';
    message = '';
    try {
      const result = await api.setHostname(hostname.trim());
      message = `The controller now answers to ${result.hostname}.local.`;
      hostname = '';
      await refresh();
    } catch (e) {
      error = describe(e);
    } finally {
      busy = '';
    }
  }
</script>

<section>
  <header class="page">
    <h2>Network</h2>
    <p class="lede">
      Join the controller to the lab or house Wi-Fi so it can be opened from any
      device on the same router. Its own access point stays available as the way
      back in.
    </p>
  </header>

  {#if error}<p class="error">{error}</p>{/if}
  {#if message}<p class="ok">{message}</p>{/if}

  <!-- ---------------------------------------------------------------- -->
  <!-- Where the instrument is right now                                 -->
  <!-- ---------------------------------------------------------------- -->
  <article class="card">
    <h3>Current state</h3>
    {#if !status}
      <p class="muted">Reading…</p>
    {:else}
      <dl class="grid">
        <dt>Mode</dt><dd><span class="pill">{modeLabel}</span> {status.state}</dd>

        <dt>Home network</dt>
        <dd>{status.configured ? status.ssid : 'not configured'}</dd>

        <dt>Signal</dt>
        <dd class={signalClass(status.station.rssi)}>
          {signalLabel(status.station.rssi)}
        </dd>

        <dt>Local address</dt>
        <dd>
          {#if status.station.connected}
            <!-- Both offered, neither followed automatically.  The browser is
                 very likely still on the fallback AP, where neither of these
                 is reachable yet — navigating there would lose the page. -->
            <a href={`http://${status.station.ip}`}>http://{status.station.ip}</a>
            {#if status.mdns}
              <br /><a href={`http://${status.mdns}`}>http://{status.mdns}</a>
            {/if}
          {:else}
            <span class="muted">no address on the home network</span>
          {/if}
        </dd>

        <dt>Fallback access point</dt>
        <dd>
          {#if status.access_point.active}
            {status.access_point.ssid} —
            <a href={`http://${status.access_point.ip || '192.168.4.1'}`}>
              http://{status.access_point.ip || '192.168.4.1'}</a>
          {:else}
            <span class="muted">off (the home network is up)</span>
          {/if}
        </dd>

        <dt>Device name</dt><dd>{status.hostname}</dd>

        <dt>Stability</dt>
        <dd>
          {status.reconnects} connects · {status.disconnects} drops
          {#if status.last_disconnect_reason}
            · last: {status.last_disconnect_reason}
          {/if}
        </dd>

        {#if status.last_error}
          <dt>Last error</dt>
          <dd class="weak">{status.last_error.detail || status.last_error.code}</dd>
        {/if}
      </dl>
    {/if}
  </article>

  <!-- ---------------------------------------------------------------- -->
  <!-- Joining                                                           -->
  <!-- ---------------------------------------------------------------- -->
  <article class="card">
    <h3>Connect to a network</h3>

    <div class="row">
      <button onclick={scan} disabled={scanning || connecting}>
        {scanning ? 'Scanning…' : 'Find networks'}
      </button>
      <label class="inline">
        <input type="checkbox" bind:checked={hidden} />
        Hidden network (type the name myself)
      </label>
    </div>

    {#if networks.length > 0 && !hidden}
      <ul class="networks">
        {#each networks as candidate (candidate.ssid)}
          <li>
            <button class="pick" class:chosen={candidate.ssid === ssid}
                    onclick={() => choose(candidate)}>
              <span class="name">{candidate.ssid}</span>
              <span class="meta {signalClass(candidate.rssi)}">
                {signalLabel(candidate.rssi)}
                · ch {candidate.channel}
                · {candidate.secured ? 'secured' : 'open'}
              </span>
            </button>
          </li>
        {/each}
      </ul>
    {/if}

    <label>
      Network name (SSID)
      <input bind:value={ssid} placeholder="HomeWiFi" autocomplete="off" />
    </label>

    <label>
      Password
      <span class="password">
        <!-- Deliberately never pre-filled.  The controller does not send the
             stored password back, so showing dots here would imply a value
             that does not exist and invite the operator to leave it alone. -->
        {#if showPassword}
          <input bind:value={password} autocomplete="off" spellcheck="false"
                 placeholder={status?.password_set ? 'leave empty to keep the saved one' : ''} />
        {:else}
          <input type="password" bind:value={password} autocomplete="off"
                 placeholder={status?.password_set ? 'leave empty to keep the saved one' : ''} />
        {/if}
        <button type="button" class="ghost"
                onclick={() => (showPassword = !showPassword)}>
          {showPassword ? 'Hide' : 'Show'}
        </button>
      </span>
    </label>

    {#if status && !status.password_set && status.configured === false}
      <p class="note">
        Joining a shared network makes this controller reachable by everything
        on it. Set a device password on the System page first if the network is
        not yours alone.
      </p>
    {/if}

    <div class="row">
      <button class="primary" onclick={connect}
              disabled={!ssid.trim() || busy === 'connect' || connecting}>
        {connecting ? 'Connecting…' : 'Test and connect'}
      </button>
      {#if connecting}
        <span class="muted">
          Trying the network. The access point stays up while this runs — if the
          password is wrong nothing is lost.
        </span>
      {/if}
    </div>
  </article>

  <!-- ---------------------------------------------------------------- -->
  <!-- Name and the way back                                             -->
  <!-- ---------------------------------------------------------------- -->
  <article class="card">
    <h3>Device name</h3>
    <p class="muted">
      The controller answers to <code>{status?.hostname ?? '…'}.local</code> on
      the home network. Lower case letters, digits and hyphens.
    </p>
    <div class="row">
      <input bind:value={hostname} placeholder={status?.hostname ?? 'lab-reactor'}
             autocomplete="off" />
      <button onclick={renameHost}
              disabled={!hostname.trim() || busy === 'hostname'}>Rename</button>
    </div>
  </article>

  <article class="card danger">
    <h3>Forget the home network</h3>
    <p class="muted">
      The controller goes back to its own access point and stays there until a
      network is configured again. Sensors, experiments and logging are not
      affected.
    </p>
    {#if confirmingForget}
      <p class="note">
        The browser will lose this page if it is connected over the home
        network. Reconnect to
        <strong>{status?.access_point.ssid ?? 'LAB-CONTROLLER'}</strong> and open
        <strong>http://192.168.4.1</strong>.
      </p>
      <div class="row">
        <button class="danger" onclick={forget} disabled={busy === 'forget'}>
          Yes, forget it
        </button>
        <button onclick={() => (confirmingForget = false)}>Cancel</button>
      </div>
    {:else}
      <button onclick={() => (confirmingForget = true)}
              disabled={!status?.configured}>
        Forget home network
      </button>
    {/if}
  </article>
</section>

<style>
  section { display: grid; gap: 1rem; max-width: 760px; }
  .page h2 { margin: 0 0 0.2rem; }
  .lede { margin: 0; color: var(--muted); max-width: 62ch; }
  .card { border: 1px solid var(--line); border-radius: 8px; padding: 1rem;
          display: grid; gap: 0.7rem; background: var(--surface-1); }
  .card.danger { border-color: color-mix(in srgb, var(--danger) 40%, var(--line)); }
  h3 { margin: 0; font-size: 0.95rem; }
  .grid { display: grid; grid-template-columns: 11rem 1fr; gap: 0.4rem 1rem;
          margin: 0; }
  dt { color: var(--muted); }
  dd { margin: 0; overflow-wrap: anywhere; }
  .pill { border: 1px solid var(--line); border-radius: 999px;
          padding: 0.05rem 0.5rem; font-size: 0.78rem; }
  .row { display: flex; gap: 0.5rem; align-items: center; flex-wrap: wrap; }
  .inline { display: flex; align-items: center; gap: 0.4rem; width: auto; }
  /* The page-wide `input { width: 100% }` stretches a checkbox into a bar. */
  .inline input[type=checkbox] { width: auto; }
  .networks { list-style: none; margin: 0; padding: 0; display: grid;
              gap: 0.25rem; max-height: 15rem; overflow-y: auto; }
  /* A list of choices, not a stack of buttons: four full-strength buttons in a
     row dominated the card and made the network names harder to scan, which is
     the one thing this list exists for. */
  .pick { width: 100%; display: flex; justify-content: space-between;
          gap: 1rem; text-align: left; background: var(--surface-2);
          color: var(--text); border-color: transparent; }
  .pick:hover { border-color: var(--line); }
  .pick.chosen { border-color: var(--accent); color: var(--accent);
                 background: var(--surface-2); }
  .name { font-weight: 500; }
  .meta { color: var(--muted); font-size: 0.8rem; white-space: nowrap; }
  .password { display: flex; gap: 0.4rem; }
  .password input { flex: 1; }
  .ghost { width: auto; white-space: nowrap; }
  .note { margin: 0; padding: 0.5rem 0.7rem; border-radius: 6px;
          background: var(--surface-2); color: var(--muted); font-size: 0.85rem; }
  .muted { color: var(--muted); }
  .good { color: var(--ok, #3fb950); }
  .fair { color: var(--warn, #d29922); }
  .weak { color: var(--danger, #f85149); }
  .error { color: var(--danger); margin: 0; }
  .ok { color: var(--ok, #3fb950); margin: 0; }
  code { font-size: 0.85em; }
</style>
