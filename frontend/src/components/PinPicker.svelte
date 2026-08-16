<script lang="ts">
  // ===========================================================================
  //  PinPicker — the pin selector.
  //
  //  It renders what GET /api/v1/gpio says and derives NOTHING itself.  The
  //  firmware already knows which pins exist, which have no output driver,
  //  which are wired to the flash chip, which belong to an I²C bus and which
  //  ADC channel they sit on.  Re-implementing any of that here would create a
  //  second source of truth that is guaranteed to drift the moment a new chip
  //  is supported.
  // ===========================================================================
  import type { GpioMap, GpioPin } from '../lib/api';

  let {
    value = $bindable<number | undefined>(undefined),
    pinUse = 'digital_input',
    gpio = null,
    ownerDevice = undefined,
    invalid = false,
  }: {
    value?: number;
    pinUse?: string;
    gpio?: GpioMap | null;
    /** Handle of the device being edited, so its own pins stay selectable. */
    ownerDevice?: number;
    invalid?: boolean;
  } = $props();

  interface Option {
    pin: number;
    label: string;
    disabled: boolean;
    note: string;
  }

  const options = $derived.by<Option[]>(() => {
    if (!gpio) return [];
    return gpio.pins.map((pin) => describe(pin)).filter((o) => o !== null) as Option[];
  });

  function describe(pin: GpioPin): Option | null {
    // Physically impossible for this role — show it greyed out with the reason
    // rather than hiding it, so "why isn't GPIO34 in the list?" never comes up.
    let disabled = false;
    let note = '';

    if (!pin.usable) {
      disabled = true;
      note = pin.advisory ?? 'reserved';
    } else if ((pinUse === 'digital_output' || pinUse === 'pwm_output') && pin.input_only) {
      disabled = true;
      note = 'input only';
    } else if (pinUse === 'analog_input' && pin.adc1 === undefined) {
      disabled = true;
      note = pin.adc2 !== undefined ? 'ADC2 — unusable with Wi-Fi' : 'no ADC';
    }

    // Taken by someone else.  A pin this device already owns stays selectable,
    // otherwise "change the gain, keep the pins" would conflict with itself.
    //
    // `ownerDevice` must be undefined when no device is being edited: device
    // handle 0 means "owned by the system" in the firmware, so defaulting to 0
    // made every bus pin (I2C0 SDA and friends) look self-owned and therefore
    // selectable — exactly the pins most likely to be picked by mistake.
    if (!disabled && pin.owner &&
        (ownerDevice === undefined || pin.owner_device !== ownerDevice)) {
      disabled = true;
      note = `used by ${pin.owner}`;
    }

    // Legal but consequential.  Keep the firmware's own wording — "strapping
    // pin (MTDI): must be LOW at reset on 3.3 V flash" is actionable, the bare
    // words "strapping pin" are not.
    if (!disabled && pin.strapping) note = pin.advisory ?? 'strapping pin';

    return {
      pin: pin.pin,
      label: `GPIO${pin.pin}`,
      disabled,
      note,
    };
  }

  const selected = $derived(options.find((o) => o.pin === value));
</script>

<div class="picker" class:invalid>
  <select bind:value>
    <option value={undefined}>— select pin —</option>
    {#each options as option (option.pin)}
      <option value={option.pin} disabled={option.disabled}>
        {option.label}{option.note ? ` — ${option.note}` : ''}
      </option>
    {/each}
  </select>

  {#if selected?.note}
    <!-- A strapping pin is legal but affects boot mode; the operator is told,
         not silently allowed to shoot themselves in the foot. -->
    <span class="advisory">⚠ {selected.note}</span>
  {/if}
  {#if !gpio}
    <span class="advisory">pin map not loaded</span>
  {/if}
</div>

<style>
  .picker { display: grid; gap: 0.2rem; }
  .advisory { font-size: 0.72rem; color: var(--warn); }
  .invalid select { border-color: var(--danger); }
  option:disabled { color: var(--muted); }
</style>
