<script lang="ts">
  // ==========================================================================
  //  CloudView — sending finished segments to Yandex Disk (M17).
  //
  //  Three things on this page exist because of the milestone's one rule — a
  //  local file is deleted only after the cloud copy has been read back and
  //  matched:
  //
  //   * The queue shows what is STILL ONLY HERE.  A number that goes down is
  //     the operator's evidence that data left the device safely; a number that
  //     stops going down is the warning that it did not.
  //   * The Device Code panel shows a code and a link, and never a Yandex
  //     password field.  The controller polls Yandex itself, so this works with
  //     the browser closed — which is the entire point of the feature.
  //   * The client secret box is write-only and starts empty. The controller
  //     cannot send one back, so showing dots would imply a value that the page
  //     does not have.
  // ==========================================================================
  import { onMount } from 'svelte';
  import { api } from '../lib/api';
  import { describe } from '../lib/state.svelte';
  import { formatBytes } from '../lib/format';
  import type { CloudStatus } from '../lib/types';

  let status = $state<CloudStatus | null>(null);
  let error = $state('');
  let message = $state('');
  let busy = $state('');

  let clientId = $state('');
  let clientSecret = $state('');
  let rootPath = $state('');
  let showSecret = $state(false);

  let linkCode = $state('');
  let linkUrl = $state('');
  let linkExpiresIn = $state(0);
  let copied = $state(false);

  let confirmingDisconnect = $state(false);
  let disconnectPassword = $state('');

  const ACTIVE_POLL_MS = 1000;
  const IDLE_POLL_MS = 5000;
  let timer: ReturnType<typeof setTimeout> | null = null;

  const linking = $derived(status?.linkState === 'WAITING_USER'
                        || status?.linkState === 'REQUESTING_CODE');
  const working = $derived(status?.state === 'UPLOADING' || linking);

  /** Why the uploader is idle, in the operator's terms rather than the state
   *  machine's.  "Waiting" with no reason is the least useful thing a queue can
   *  say. */
  const explanation = $derived.by(() => {
    if (!status) return '';
    if (!status.enabled) return 'Automatic upload is off.';
    if (status.queue.corrupt) {
      return 'The upload queue could not be read, so nothing is being sent and '
           + 'no file has been deleted. ' + (status.queue.error ?? '');
    }
    if (status.queue.paused) return 'Paused. Nothing is being sent.';
    if (!status.configured) return 'Enter the OAuth application details below.';
    if (!status.authorized) return 'The Yandex account is not linked yet.';
    if (!status.networkReady) {
      return 'Waiting for the home network. The controller’s own access '
           + 'point is not a route to the internet, so nothing is attempted '
           + 'until it joins a router.';
    }
    if (!status.timeReady) {
      return 'Waiting for the clock. Certificates cannot be checked without a '
           + 'real date, and the alternative would be not checking them.';
    }
    if (status.queue.pending === 0) return 'Everything closed so far has been sent.';
    return '';
  });

  async function refresh() {
    try {
      const wasWaiting = linking;
      status = await api.cloud();
      error = '';

      // Cleared from the STATUS, not from a poll guarded by `linking` — the
      // moment the link succeeds `linking` is already false, so a branch inside
      // it never runs and the code panel sits there counting down under an
      // account that is connected.  A prompt still on screen after the thing it
      // asked for has happened is an instruction to do it again.
      if (linkCode && status.linkState === 'AUTHORIZED') {
        linkCode = '';
        if (wasWaiting) {
          message = 'The Yandex account is linked. Uploads will continue with '
                  + 'the browser closed.';
        }
      } else if (linkCode && status.linkState === 'EXPIRED') {
        linkCode = '';
        error = 'The code expired before it was entered. Start again.';
      } else if (linkCode) {
        const link = await api.cloudLinkStatus();
        linkExpiresIn = link.expiresIn;
      }
    } catch (e) {
      error = describe(e);
    }
    if (timer !== null) clearTimeout(timer);
    timer = setTimeout(refresh, working ? ACTIVE_POLL_MS : IDLE_POLL_MS);
  }

  onMount(() => {
    void refresh();
    return () => { if (timer !== null) clearTimeout(timer); };
  });

  async function save(extra: Record<string, unknown> = {}) {
    busy = 'save';
    error = '';
    message = '';
    try {
      status = await api.saveCloudConfig({
        ...(clientId.trim() ? { clientId: clientId.trim() } : {}),
        ...(clientSecret ? { clientSecret } : {}),
        ...(rootPath.trim() ? { rootPath: rootPath.trim() } : {}),
        ...extra,
      });
      // Cleared as soon as it has been sent: there is no reason for a secret to
      // stay in a DOM node on a shared bench machine.
      clientSecret = '';
      message = 'Saved.';
    } catch (e) {
      error = describe(e);
    } finally {
      busy = '';
    }
  }

  async function link() {
    busy = 'link';
    error = '';
    message = '';
    try {
      const prompt = await api.beginCloudLink();
      linkCode = prompt.userCode;
      linkUrl = prompt.verificationUrl;
      linkExpiresIn = prompt.expiresIn;
      copied = false;
      await refresh();
    } catch (e) {
      error = describe(e);
    } finally {
      busy = '';
    }
  }

  async function copyCode() {
    try {
      await navigator.clipboard.writeText(linkCode);
      copied = true;
    } catch {
      // A browser that refuses the clipboard is not a failure worth shouting
      // about — the code is on screen and can be typed.
      copied = false;
    }
  }

  async function test() {
    busy = 'test';
    error = '';
    message = '';
    try {
      await api.testCloudAccess();
      message = 'The account answered and the folder is ready.';
    } catch (e) {
      error = describe(e);
    } finally {
      busy = '';
    }
  }

  async function disconnect() {
    busy = 'disconnect';
    error = '';
    try {
      const result = await api.disconnectCloud(disconnectPassword);
      message = result.revokedRemotely
        ? 'Disconnected. The token was revoked at Yandex as well.'
        : 'Disconnected locally. Yandex could not be reached, so revoke the '
          + 'access in your Yandex account as well.';
      confirmingDisconnect = false;
      disconnectPassword = '';
      await refresh();
    } catch (e) {
      error = describe(e);
    } finally {
      busy = '';
    }
  }

  async function pause(next: boolean) {
    try {
      await api.pauseCloudQueue(next);
      await refresh();
    } catch (e) {
      error = describe(e);
    }
  }

  async function retry(jobId: number) {
    try {
      await api.retryCloudJob(jobId);
      await refresh();
    } catch (e) {
      error = describe(e);
    }
  }

  function jobLabel(state: string): string {
    switch (state) {
      case 'ACKNOWLEDGED': return 'in the cloud';
      case 'REMOTE_CONFLICT': return 'name already taken';
      case 'PAUSED_NO_AUTH': return 'needs the account re-linked';
      case 'PERMANENT_ERROR': return 'stopped';
      case 'WAITING_NETWORK': return 'waiting for the network';
      case 'WAITING_TIME': return 'waiting for the clock';
      case 'UPLOADING': return 'sending';
      default: return 'queued';
    }
  }

  function jobClass(state: string): string {
    if (state === 'ACKNOWLEDGED') return 'good';
    if (state === 'UPLOADING') return 'busy';
    if (state === 'REMOTE_CONFLICT' || state === 'PERMANENT_ERROR'
        || state === 'PAUSED_NO_AUTH') return 'weak';
    return '';
  }
</script>

<section>
  <header class="page">
    <h2>Cloud</h2>
    <p class="lede">
      Send finished log segments to Yandex Disk automatically, with the browser
      closed. A segment is removed from the controller only after the copy in
      the cloud has been read back and found identical.
    </p>
  </header>

  {#if error}<p class="error">{error}</p>{/if}
  {#if message}<p class="ok">{message}</p>{/if}

  <!-- ------------------------------------------------------------------ -->
  <article class="card">
    <h3>Queue</h3>
    {#if !status}
      <p class="muted">Reading…</p>
    {:else}
      <dl class="grid">
        <dt>State</dt>
        <dd><span class="pill">{status.state}</span></dd>

        <dt>Still on this device</dt>
        <dd class={status.queue.pending > 0 ? 'busy' : 'good'}>
          {status.queue.pending} waiting
          {#if status.queue.failed > 0}
            · <span class="weak">{status.queue.failed} stopped</span>
          {/if}
        </dd>

        {#if status.current}
          <dt>Sending</dt>
          <dd>
            {status.current.file}
            — {formatBytes(status.current.sentBytes)}
            of {formatBytes(status.current.totalBytes)}
            {#if status.current.attempt > 1}
              (attempt {status.current.attempt})
            {/if}
            <progress max={status.current.totalBytes}
                      value={status.current.sentBytes}></progress>
          </dd>
        {/if}

        {#if status.lastError}
          <dt>Last error</dt>
          <dd class="weak">{status.lastError.detail || status.lastError.code}</dd>
        {/if}
      </dl>

      {#if explanation}<p class="note">{explanation}</p>{/if}

      <div class="row">
        <button onclick={() => pause(!status!.queue.paused)}>
          {status.queue.paused ? 'Resume' : 'Pause'}
        </button>
      </div>

      {#if status.queue.jobs.length > 0}
        <table>
          <thead>
            <tr><th>Segment</th><th>Size</th><th>State</th><th></th></tr>
          </thead>
          <tbody>
            {#each status.queue.jobs as job (job.id)}
              <tr>
                <td>{job.sessionId} · {job.segmentId}</td>
                <td>{formatBytes(job.bytes)}</td>
                <td class={jobClass(job.state)}>
                  {jobLabel(job.state)}
                  {#if job.lastError}<br /><small>{job.lastError}</small>{/if}
                </td>
                <td>
                  {#if job.state !== 'ACKNOWLEDGED'}
                    <button class="small" onclick={() => retry(job.id)}>
                      Retry now
                    </button>
                  {/if}
                </td>
              </tr>
            {/each}
          </tbody>
        </table>
      {/if}
    {/if}
  </article>

  <!-- ------------------------------------------------------------------ -->
  <article class="card">
    <h3>Yandex account</h3>

    {#if linkCode}
      <div class="link">
        <p>
          1. Open <a href={linkUrl} target="_blank" rel="noreferrer">{linkUrl}</a><br />
          2. Enter this code<br />
          3. Allow access to Yandex Disk
        </p>
        <p class="code">{linkCode}</p>
        <div class="row">
          <button onclick={copyCode}>{copied ? 'Copied' : 'Copy code'}</button>
          <span class="muted">
            {linkExpiresIn > 0 ? `expires in ${linkExpiresIn} s` : 'expired'}
          </span>
        </div>
        <p class="note">
          Your Yandex password is never entered here. The controller is doing the
          waiting itself, so this finishes even if you close this page.
        </p>
      </div>
    {/if}

    <label>
      OAuth client id
      <input bind:value={clientId} autocomplete="off"
             placeholder={status?.configured ? 'stored' : ''} />
    </label>

    <label>
      OAuth client secret
      <span class="password">
        <!-- Never pre-filled: the controller has no way to send one back, so
             dots here would imply a value this page does not have. -->
        {#if showSecret}
          <input bind:value={clientSecret} autocomplete="off" spellcheck="false"
                 placeholder={status?.clientSecretSet ? 'stored — leave empty to keep it' : ''} />
        {:else}
          <input type="password" bind:value={clientSecret} autocomplete="off"
                 placeholder={status?.clientSecretSet ? 'stored — leave empty to keep it' : ''} />
        {/if}
        <button type="button" class="ghost" onclick={() => (showSecret = !showSecret)}>
          {showSecret ? 'Hide' : 'Show'}
        </button>
      </span>
    </label>

    <label>
      Folder on Yandex Disk
      <input bind:value={rootPath} autocomplete="off"
             placeholder={status?.rootPath ?? 'disk:/LabController'} />
    </label>

    <label class="inline">
      <input type="checkbox" checked={status?.enabled ?? false}
             onchange={(e) => save({ enabled: e.currentTarget.checked })} />
      Upload finished segments automatically
    </label>

    <div class="row">
      <button class="primary" onclick={() => save()} disabled={busy === 'save'}>
        Save
      </button>
      <button onclick={link} disabled={!status?.configured || busy === 'link'}>
        {status?.authorized ? 'Link again' : 'Connect Yandex Disk'}
      </button>
      <button onclick={test} disabled={!status?.authorized || busy === 'test'}>
        Check access
      </button>
    </div>

    {#if status && !status.secureStorage}
      <p class="note warn">
        This board stores the token in unencrypted flash. Anyone who can take the
        controller away can read it. Give the OAuth application access to Yandex
        Disk only — never to mail, contacts or the profile — so that the worst
        case is these CSV files rather than an account.
      </p>
    {/if}
  </article>

  {#if status?.authorized}
    <article class="card danger">
      <h3>Disconnect</h3>
      <p class="muted">
        Removes the token and the client secret from this controller. Queued
        segments stay on the device and are not deleted.
      </p>
      {#if confirmingDisconnect}
        <label>
          Controller password
          <input type="password" bind:value={disconnectPassword} autocomplete="off" />
        </label>
        <div class="row">
          <button class="danger" onclick={disconnect} disabled={busy === 'disconnect'}>
            Disconnect
          </button>
          <button onclick={() => (confirmingDisconnect = false)}>Cancel</button>
        </div>
      {:else}
        <button onclick={() => (confirmingDisconnect = true)}>
          Disconnect Yandex Disk
        </button>
      {/if}
    </article>
  {/if}
</section>

<style>
  section { display: grid; gap: 1rem; max-width: 800px; }
  .page h2 { margin: 0 0 0.2rem; }
  .lede { margin: 0; color: var(--muted); max-width: 64ch; }
  .card { border: 1px solid var(--line); border-radius: 8px; padding: 1rem;
          display: grid; gap: 0.7rem; background: var(--surface-1); }
  .card.danger { border-color: color-mix(in srgb, var(--danger) 40%, var(--line)); }
  h3 { margin: 0; font-size: 0.95rem; }
  .grid { display: grid; grid-template-columns: 12rem 1fr; gap: 0.4rem 1rem;
          margin: 0; }
  dt { color: var(--muted); }
  dd { margin: 0; overflow-wrap: anywhere; }
  .pill { border: 1px solid var(--line); border-radius: 999px;
          padding: 0.05rem 0.5rem; font-size: 0.78rem; }
  .row { display: flex; gap: 0.5rem; align-items: center; flex-wrap: wrap; }
  .inline { display: flex; align-items: center; gap: 0.4rem; width: auto; }
  .inline input[type=checkbox] { width: auto; }
  .password { display: flex; gap: 0.4rem; }
  .password input { flex: 1; }
  .ghost, .small { width: auto; white-space: nowrap; }
  .small { font-size: 0.78rem; padding: 0.15rem 0.45rem; }
  .link { border: 1px solid var(--accent); border-radius: 6px; padding: 0.8rem;
          display: grid; gap: 0.5rem; }
  .code { margin: 0; font-size: 1.8rem; letter-spacing: 0.15em;
          font-family: ui-monospace, monospace; color: var(--accent); }
  .note { margin: 0; padding: 0.5rem 0.7rem; border-radius: 6px;
          background: var(--surface-2); color: var(--muted); font-size: 0.85rem; }
  .note.warn { border-left: 3px solid var(--warn, #d29922); }
  table { width: 100%; border-collapse: collapse; font-size: 0.85rem; }
  th, td { text-align: left; padding: 0.3rem 0.4rem;
           border-bottom: 1px solid var(--line); vertical-align: top; }
  th { color: var(--muted); font-weight: 500; }
  progress { width: 100%; height: 0.5rem; }
  .muted { color: var(--muted); }
  .good { color: var(--ok, #3fb950); }
  .busy { color: var(--accent); }
  .weak { color: var(--danger, #f85149); }
  .error { color: var(--danger); margin: 0; }
  .ok { color: var(--ok, #3fb950); margin: 0; }
</style>
