<script lang="ts">
  // ===========================================================================
  //  SignInBar — who this browser is, in the application frame.
  //
  //  It lives beside the emergency stop on purpose, and it never covers it.
  //  A modal sign-in dialog over a page whose stop button is the reason
  //  somebody opened it would be the interface equivalent of putting a padlock
  //  on a fire door (ADR-0020).
  //
  //  Two states worth reading carefully:
  //    * no password set — a banner that does not go away, because an
  //      instrument on a shared network with no credential is a decision
  //      somebody should make deliberately rather than inherit;
  //    * signed out — a compact form, and the rest of the interface stays
  //      readable: reading is not what the password is for.
  // ===========================================================================
  import { api, ApiRequestError } from '../lib/api';
  import { errorSentence } from '../lib/format';
  import { controller, describe } from '../lib/state.svelte';

  let password = $state('');
  let confirmPassword = $state('');
  let busy = $state(false);
  let error = $state('');
  let showSetup = $state(false);

  const auth = $derived(controller.auth);

  async function signIn() {
    busy = true;
    error = '';
    try {
      await api.login(password);
      password = '';
      await controller.refreshAuth();
      await controller.refresh();
    } catch (e) {
      error = e instanceof ApiRequestError ? errorSentence(e.error) : describe(e);
    } finally {
      busy = false;
    }
  }

  async function signOut() {
    busy = true;
    try {
      await api.logout();
      await controller.refreshAuth();
    } catch (e) {
      error = describe(e);
    } finally {
      busy = false;
    }
  }

  async function setPassword() {
    if (password !== confirmPassword) {
      error = 'the two passwords do not match';
      return;
    }
    busy = true;
    error = '';
    try {
      await api.setPassword(password);
      // Every session ended, including this one — so the next thing the
      // operator sees is the sign-in form, which is the honest consequence.
      password = '';
      confirmPassword = '';
      showSetup = false;
      await controller.refreshAuth();
    } catch (e) {
      error = e instanceof ApiRequestError ? errorSentence(e.error) : describe(e);
    } finally {
      busy = false;
    }
  }
</script>

{#if !auth.configured}
  <div class="setup">
    <strong>This instrument has no password.</strong>
    <span class="muted small">
      Anyone on this network can change its configuration. Reading is not the
      risk; a colleague importing their rig over your run is.
    </span>
    {#if showSetup}
      <span class="form">
        <input type="password" placeholder="new password" bind:value={password}
               autocomplete="new-password" />
        <input type="password" placeholder="again" bind:value={confirmPassword}
               autocomplete="new-password" />
        <button type="button" class="primary" disabled={busy || password.length < 8}
                onclick={setPassword}>Set password</button>
      </span>
    {:else}
      <button type="button" onclick={() => (showSetup = true)}>Set a password</button>
    {/if}
    {#if error}<span class="bad small">{error}</span>{/if}
  </div>
{:else if !auth.signed_in}
  <div class="signin">
    <span class="muted small">Signed out — you can read everything and stop anything.</span>
    <input type="password" placeholder="password" bind:value={password}
           autocomplete="current-password"
           onkeydown={(e) => { if (e.key === 'Enter') void signIn(); }} />
    <button type="button" class="primary" disabled={busy || !password}
            onclick={signIn}>Sign in</button>
    {#if auth.locked}
      <span class="warn small">too many attempts — wait a minute</span>
    {/if}
    {#if error}<span class="bad small">{error}</span>{/if}
  </div>
{:else}
  <button type="button" class="out" disabled={busy} onclick={signOut}>Sign out</button>
{/if}

<style>
  .setup, .signin { display: flex; align-items: center; gap: 0.5rem;
                    flex-wrap: wrap; }
  .setup { background: color-mix(in srgb, var(--warn) 14%, transparent);
           border: 1px solid var(--warn); border-radius: 8px;
           padding: 0.5rem 0.7rem; font-size: 0.8rem; }
  .setup .muted { flex: 1 1 18rem; }
  .form { display: flex; gap: 0.4rem; }
  input { background: var(--surface-2); border: 1px solid var(--line);
          color: var(--text); border-radius: 5px; padding: 0.2rem 0.4rem;
          font: inherit; font-size: 0.8rem; }
  button { background: var(--surface-2); border: 1px solid var(--line);
           color: var(--text); border-radius: 6px; padding: 0.2rem 0.6rem;
           cursor: pointer; font: inherit; font-size: 0.78rem; }
  button.primary { background: var(--accent); border-color: var(--accent);
                   color: #05121f; font-weight: 600; }
  button:disabled { opacity: 0.5; cursor: default; }
  button.out { font-size: 0.72rem; }
  .muted { color: var(--muted); }
  .small { font-size: 0.72rem; }
  .warn { color: var(--warn); }
  .bad { color: var(--danger); }
</style>
