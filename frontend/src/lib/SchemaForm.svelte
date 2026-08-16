<script lang="ts">
  // ===========================================================================
  //  SchemaForm — renders a module's entire configuration form from its
  //  manifest.  This component is the frontend half of §7 and §63: adding a
  //  new sensor to the firmware must not require touching the UI.
  //
  //  If you ever find yourself writing `if (module.id === 'hx711')` anywhere in
  //  the frontend, the manifest is missing something — extend ParamSpec
  //  instead.
  // ===========================================================================
  import { untrack } from 'svelte';
  import PinPicker from '../components/PinPicker.svelte';
  import type { GpioMap } from './api';
  import type { ModuleManifest, ParamSpec } from './types';

  let {
    manifest,
    value = $bindable<Record<string, unknown>>({}),
    gpio = null,
    ownerDevice = undefined,
    showAdvanced = $bindable(false),
    errorField = '',
    errorText = '',
    handledFields = [],
  }: {
    manifest: ModuleManifest;
    value: Record<string, unknown>;
    gpio?: GpioMap | null;
    /** Handle of the device being edited; undefined when creating a new one. */
    ownerDevice?: number;
    showAdvanced?: boolean;
    errorField?: string;
    errorText?: string;
    /** Fields the parent renders itself, so this form does not report them twice. */
    handledFields?: string[];
  } = $props();

  // Seed defaults ONCE PER MODULE, so the form opens pre-filled exactly as the
  // firmware would interpret an omitted key.  The write-back must not be a
  // reactive dependency: re-running on every edit would make a cleared field
  // snap straight back to its default and become impossible to empty.
  $effect(() => {
    const params = manifest.params;
    untrack(() => {
      for (const spec of params) {
        if (value[spec.key] !== undefined || spec.default === undefined) continue;
        // Coerce by declared type.  `default` is textual in the manifest, and
        // seeding a numeric field with the string "5" makes every saved
        // configuration carry quoted numbers — which the firmware accepts and
        // which then look wrong in every exported file.
        value[spec.key] =
          spec.type === 'int' || spec.type === 'float'
            ? Number(spec.default)
            : spec.type === 'bool'
              ? spec.default === 'true'
              : spec.default;
      }
    });
  });

  // An error about a parameter the operator cannot see is not a message, it is
  // a locked door.  Open the advanced group when the firmware objects to a
  // field inside it...
  $effect(() => {
    const offending = manifest.params.find((p) => p.key === errorField);
    if (offending?.advanced) showAdvanced = true;
  });

  // ...and if the offending field is not on this form at all (`key`, `bus`, or
  // a parameter hidden by visible_if), say so where it can be read.
  const orphanError = $derived.by(() => {
    if (!errorText || !errorField) return '';
    if (handledFields.includes(errorField)) return '';
    const spec = manifest.params.find((p) => p.key === errorField);
    if (spec && isVisible(spec)) return '';
    return spec ? `${spec.label}: ${errorText}` : `${errorField}: ${errorText}`;
  });

  /** Supports the simple `key=value` / `key!=value` conditions in ParamSpec. */
  function isVisible(spec: ParamSpec): boolean {
    if (spec.advanced && !showAdvanced) return false;
    if (!spec.visible_if) return true;
    const negated = spec.visible_if.includes('!=');
    const [key, expected] = spec.visible_if.split(negated ? '!=' : '=');
    const actual = String(value[key!.trim()] ?? '');
    return negated ? actual !== expected!.trim() : actual === expected!.trim();
  }

  const hasAdvanced = $derived(manifest.params.some((p) => p.advanced));
</script>

<div class="schema-form">
  {#if orphanError}
    <p class="error orphan">{orphanError}</p>
  {/if}
  {#each manifest.params as spec (spec.key)}
    {#if isVisible(spec)}
      <label class="field" class:invalid={errorField === spec.key}>
        <span class="label">
          {spec.label}
          {#if spec.unit}<em class="unit">{spec.unit}</em>{/if}
          {#if !spec.required}<em class="optional">optional</em>{/if}
        </span>

        {#if spec.type === 'select'}
          <select bind:value={value[spec.key]}>
            {#each spec.options ?? [] as option (option.value)}
              <option value={option.value}>{option.label}</option>
            {/each}
          </select>

        {:else if spec.type === 'gpio'}
          <PinPicker
            bind:value={value[spec.key] as number | undefined}
            pinUse={spec.pin_use ?? 'digital_input'}
            {gpio}
            {ownerDevice}
            invalid={errorField === spec.key} />

        {:else if spec.type === 'bool'}
          <input type="checkbox" bind:checked={value[spec.key] as boolean} />

        {:else if spec.type === 'int' || spec.type === 'float'}
          <input
            type="number"
            min={spec.min}
            max={spec.max}
            step={spec.step ?? (spec.type === 'int' ? 1 : 'any')}
            bind:value={value[spec.key]} />

        {:else if spec.type === 'i2c_address'}
          <input type="text" placeholder="0x76" bind:value={value[spec.key]} />

        {:else}
          <input type="text" bind:value={value[spec.key]} />
        {/if}

        {#if errorField === spec.key && errorText}
          <span class="error">{errorText}</span>
        {:else if spec.help}
          <span class="help">{spec.help}</span>
        {/if}
      </label>
    {/if}
  {/each}

  {#if hasAdvanced}
    <button type="button" class="toggle" onclick={() => (showAdvanced = !showAdvanced)}>
      {showAdvanced ? 'Hide' : 'Show'} advanced options
    </button>
  {/if}
</div>

<style>
  .schema-form { display: grid; gap: 0.75rem; }
  .field { display: grid; gap: 0.25rem; }
  .label { font-size: 0.72rem; letter-spacing: 0.05em; text-transform: uppercase; opacity: 0.75; }
  .unit, .optional { font-style: normal; opacity: 0.55; margin-left: 0.35rem; font-size: 0.7rem; }
  .help { font-size: 0.72rem; opacity: 0.6; }
  .orphan { margin: 0; }
  .error { font-size: 0.72rem; color: var(--danger); }
  .invalid input, .invalid select { border-color: var(--danger); }
  .toggle { justify-self: start; background: none; border: 0; padding: 0; font: inherit;
            color: var(--accent); cursor: pointer; }
</style>
