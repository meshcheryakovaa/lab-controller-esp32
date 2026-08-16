// =============================================================================
//  state.svelte.ts — everything the UI knows about the controller.
//
//  Split by lifetime, matching how the firmware serves it:
//    * DESCRIPTORS (modules, devices, channels, GPIO map) change rarely and
//      come from REST.  They are re-fetched when `config_revision` moves.
//    * VALUES arrive only over the WebSocket, addressed by handle, and never
//      touch REST.  A dashboard open for eight hours makes zero HTTP requests.
//
//  History for charts lives here as a fixed-size ring per channel: the browser
//  is the only place with room for it, and re-requesting it from a 240 MHz MCU
//  every time a chart mounts would be absurd.
// =============================================================================

import { api, ApiRequestError, type GpioMap } from './api';
import { kUnknownRevision, live } from './live';
import type {
  AuthStatus, Calibration, Channel, ChannelQuality, ControlDocument, Device,
  LoggingStatus, ModuleManifest, RunStatus,
} from './types';

export interface Alert {
  id: number;
  severity: number;
  code: string;
  message: string;
  at: number;
}

const HISTORY_POINTS = 900; // 3 minutes at 5 Hz — enough to see a transient

class ChannelHistory {
  readonly time: number[] = [];
  readonly value: number[] = [];

  push(t: number, v: number): void {
    this.time.push(t);
    this.value.push(v);
    if (this.time.length > HISTORY_POINTS) {
      this.time.shift();
      this.value.shift();
    }
  }
}

export class ControllerState {
  // --- descriptors ---------------------------------------------------------
  modules = $state<ModuleManifest[]>([]);
  devices = $state<Device[]>([]);
  channels = $state<Channel[]>([]);
  gpio = $state<GpioMap | null>(null);
  /** Every calibration version of every channel; small, and the Channels page
   *  needs the active one per row. */
  calibrations = $state<Calibration[]>([]);
  /** Whether the safety layer is holding every output at its safe value. */
  outputsTripped = $state(false);
  // Loops, rules and limits.  Held here rather than in the Control page because
  // a latched interlock has to be visible from every screen — the operator who
  // needs to know is not necessarily the one who was looking at it (§30).
  control = $state<ControlDocument | null>(null);
  // The running scenario.  Held here, like the control document, because a run
  // in progress is something every screen may need to say out loud.
  run = $state<RunStatus | null>(null);
  // Whether the rig is recording, and how much room is left.  Frame-level,
  // like the trip state: a dataset that stopped because the card filled is
  // something every screen should be able to say.
  logging = $state<LoggingStatus | null>(null);
  // Who this browser is allowed to be.  Frame-level: the sign-in prompt and the
  // "no password set" banner belong to the application, not to a page.
  auth = $state<AuthStatus>({ configured: false, signed_in: true, sessions: 0,
                              locked: false });
  tripReason = $state('');
  outputCount = $state(0);
  system = $state<Record<string, any> | null>(null);
  diagnostics = $state<Record<string, any> | null>(null);

  // --- live ----------------------------------------------------------------
  connected = $state(false);
  values = $state<Record<number, number>>({});
  quality = $state<Record<number, ChannelQuality>>({});
  lastFrameAt = $state(0);

  // --- ui ------------------------------------------------------------------
  loading = $state(false);
  loadError = $state<string | null>(null);
  alerts = $state<Alert[]>([]);

  private history = new Map<number, ChannelHistory>();
  /**
   * Which channel key each handle stood for the last time we looked.  Handles
   * are slot indices in the firmware, so deleting a channel and adding another
   * hands the same number to a completely different measurement — without this
   * check the new channel would inherit the old one's chart history and its
   * last value.
   */
  private handleOwner = new Map<number, string>();
  private configRevision = -1;
  private controlWatchers = 0;
  private controlTimer: ReturnType<typeof setInterval> | null = null;
  private runWatchers = 0;
  private runTimer: ReturnType<typeof setInterval> | null = null;
  private alertSeq = 0;

  historyFor(handle: number): ChannelHistory {
    let entry = this.history.get(handle);
    if (!entry) {
      entry = new ChannelHistory();
      this.history.set(handle, entry);
    }
    return entry;
  }

  /**
   * The quality to DISPLAY for a channel, which is not always the quality the
   * firmware last reported: nothing can be fresher than the link that carries
   * it.  With the socket down the newest value we hold is a snapshot of an
   * unknown age, and calling it GOOD would be a lie the operator cannot check.
   */
  qualityOf(handle: number): ChannelQuality {
    const reported = this.quality[handle];
    if (reported === undefined) return 'UNKNOWN';
    if (!this.connected && reported === 'GOOD') return 'STALE';
    return reported;
  }

  /** The calibration a channel is actually running, if any. */
  activeCalibration(channelKey: string): Calibration | undefined {
    return this.calibrations.find((c) => c.active && c.channel === channelKey);
  }

  calibrationsFor(channelKey: string): Calibration[] {
    return this.calibrations.filter((c) => c.channel === channelKey);
  }

  channelByKey(key: string): Channel | undefined {
    return this.channels.find((c) => c.key === key);
  }

  channelByHandle(handle: number): Channel | undefined {
    return this.channels.find((c) => c.handle === handle);
  }

  deviceByHandle(handle: number): Device | undefined {
    return this.devices.find((d) => d.handle === handle);
  }

  moduleById(id: string): ModuleManifest | undefined {
    return this.modules.find((m) => m.id === id);
  }

  // --- loading -------------------------------------------------------------
  async loadStatic(): Promise<void> {
    // Manifests are genuinely static: they are compiled into the firmware and
    // cannot change without a reflash.
    this.modules = (await api.modules()).modules;
  }

  async refresh(): Promise<void> {
    this.loading = true;
    this.loadError = null;
    try {
      const [system, devices, channels, gpio, calibrations, outputs, control, run,
             logging, auth] = await Promise.all([
        api.system(),
        api.devices(),
        // Ask for values too: a freshly loaded page then shows real readings
        // immediately instead of dashes until the first telemetry frame, and
        // it still works if the socket cannot be established at all.
        api.channels(true),
        // The pin map is NOT static: every device that starts or stops changes
        // who owns what.  Fetching it once at boot left the pin picker offering
        // pins that were already taken and the Hardware page claiming nothing
        // was claimed.
        api.gpio(),
        // Small file, and every row of the Channels page wants to know whether
        // the number in it means grams or ADC counts.
        api.calibrations().catch(() => ({ calibrations: [] })),
        // Cheap, and the master stop must be reachable from every screen —
        // including the ones that are not about outputs.
        api.outputs().catch(() => ({ tripped: false, reason: '', outputs: [] })),
        api.control().catch(() => null),
        api.runState().catch(() => null),
        api.logs().then((response) => response.recording).catch(() => null),
        api.auth().catch(() => ({ configured: false, signed_in: true,
                                  sessions: 0, locked: false })),
      ]);
      this.system = system;
      this.gpio = gpio;
      this.calibrations = calibrations.calibrations;
      this.outputsTripped = outputs.tripped;
      this.tripReason = outputs.reason ?? '';
      this.outputCount = outputs.outputs?.length ?? 0;
      this.control = control;
      this.run = run;
      this.logging = logging;
      this.auth = auth;
      this.devices = devices.devices;
      this.channels = channels.channels;
      this.forgetStaleHandles(channels.channels);
      for (const channel of channels.channels) {
        if (!channel.value) continue;
        this.values[channel.handle] = channel.value.processed;
        this.quality[channel.handle] = channel.value.quality;
        this.historyFor(channel.handle).push(channel.value.t, channel.value.processed);
      }
      this.configRevision = Number(system.config_revision ?? -1);
      this.subscribeToVisibleChannels();
    } catch (error) {
      this.loadError = describe(error);
    } finally {
      this.loading = false;
    }
  }

  /**
   * Drop history, values and quality for handles that no longer exist or that
   * now belong to a different channel.  Called on every descriptor refresh —
   * which is exactly when the rig can have changed underneath us.
   */
  private forgetStaleHandles(current: Channel[]): void {
    const owner = new Map<number, string>();
    for (const channel of current) owner.set(channel.handle, channel.key);

    for (const [handle, key] of this.handleOwner) {
      if (owner.get(handle) === key) continue;
      this.history.delete(handle);
      delete this.values[handle];
      delete this.quality[handle];
    }
    this.handleOwner = owner;
  }

  /**
   * Re-reads raw / calibrated / processed for every channel.  The Channels page
   * shows the three stages side by side to separate "the sensor is dead" from
   * "the calibration is wrong", and only `processed` comes over the socket —
   * so without this poll the first two columns are a snapshot from page load
   * displayed next to a live number, which is worse than not showing them.
   */
  /**
   * Re-reads the control document.  Separate from refresh() because the Control
   * page needs it every couple of seconds and the device list does not.
   */
  async refreshControl(): Promise<void> {
    try {
      this.control = await api.control();
      // The control document carries the trip state, so take it: an interlock
      // that fires while somebody is looking at the Control page used to latch,
      // stop every output, and leave the master-stop banner in the application
      // frame showing nothing at all until the next full refresh.  The one
      // screen most likely to be open when a rig stops itself was the one
      // screen that did not say so.
      this.outputsTripped = this.control.tripped ?? this.outputsTripped;
      this.tripReason = this.control.trip_reason ?? this.tripReason;
    } catch (error) {
      this.loadError = describe(error);
    }
  }

  /**
   * Keeps the control document fresh while anything is looking at it, and stops
   * as soon as nothing is.  Ref-counted because a loop widget on a dashboard
   * needs exactly the same poll the Control page does, and a dashboard with
   * four of them must not open four of them.
   *
   * Without this a loop widget rendered its measurement once and then never
   * again: a live regulator displayed as a still photograph, which is the same
   * failure the socket-down STALE marking exists to prevent.
   */
  watchControl(): () => void {
    this.controlWatchers += 1;
    void this.refreshControl();
    this.controlTimer ??= setInterval(() => void this.refreshControl(), 2000);
    return () => {
      this.controlWatchers = Math.max(0, this.controlWatchers - 1);
      if (this.controlWatchers === 0 && this.controlTimer !== null) {
        clearInterval(this.controlTimer);
        this.controlTimer = null;
      }
    };
  }

  async refreshAuth(): Promise<void> {
    try {
      this.auth = await api.auth();
    } catch {
      // A build without credentials answers NOT_SUPPORTED; treat that as open
      // rather than as locked, which is what it is.
      this.auth = { configured: false, signed_in: true, sessions: 0, locked: false };
    }
  }

  /** True when a write would be refused — used to disable controls rather than
      let somebody fill in a form that cannot be submitted. */
  get readOnly(): boolean {
    return this.auth.configured && !this.auth.signed_in;
  }

  async refreshLogging(): Promise<void> {
    try {
      this.logging = (await api.logs()).recording;
    } catch {
      this.logging = null;
    }
  }

  async refreshRun(): Promise<void> {
    try {
      this.run = await api.runState();
    } catch {
      // A build without the experiment engine answers NOT_SUPPORTED; that is a
      // missing feature, not a fault worth putting on screen.
      this.run = null;
    }
  }

  /** Same ref-counted poll as watchControl(), for the same reason. */
  watchRun(): () => void {
    this.runWatchers += 1;
    void this.refreshRun();
    this.runTimer ??= setInterval(() => void this.refreshRun(), 1000);
    return () => {
      this.runWatchers = Math.max(0, this.runWatchers - 1);
      if (this.runWatchers === 0 && this.runTimer !== null) {
        clearInterval(this.runTimer);
        this.runTimer = null;
      }
    };
  }

  get runningExperiment(): boolean {
    return this.run?.state === 'RUNNING' || this.run?.state === 'PAUSED';
  }

  get latchedLimits(): number {
    return this.control?.limits.filter((limit) => limit.latched).length ?? 0;
  }

  async refreshChannelValues(): Promise<void> {
    try {
      const fresh = await api.channels(true);
      // Output state rides on the channel, so this poll keeps the countdown on
      // a control widget honest without a second request.
      const byHandle = new Map(fresh.channels.map((c) => [c.handle, c]));
      for (const channel of this.channels) {
        const update = byHandle.get(channel.handle);
        // Handles are reused slots; a different key in the same slot means the
        // channel we knew is gone, not that it changed value.
        if (!update || update.key !== channel.key) continue;
        channel.value = update.value;
        channel.output = update.output;
        if (!update.value) continue;
        this.values[channel.handle] = update.value.processed;
        this.quality[channel.handle] = update.value.quality;
      }
      this.loadError = null;
    } catch (error) {
      this.loadError = describe(error);
    }
  }

  private diagnosticsInFlight = false;

  async refreshDiagnostics(): Promise<void> {
    // The board serves HTTP from one task.  Stacking requests on a slow reply
    // would queue against the same socket the telemetry uses, and the answers
    // could be applied out of order.
    if (this.diagnosticsInFlight) return;
    this.diagnosticsInFlight = true;
    try {
      this.diagnostics = await api.diagnostics();
      this.loadError = null;
    } catch (error) {
      this.loadError = describe(error);
    } finally {
      this.diagnosticsInFlight = false;
    }
  }

  /** Ask the firmware for exactly the channels we can display, and no others. */
  subscribeToVisibleChannels(): void {
    live.subscribe(this.channels.filter((c) => c.visible).map((c) => c.handle));
  }

  // --- wiring --------------------------------------------------------------
  start(): () => void {
    const offStatus = live.onStatus((state) => {
      this.connected = state;
      if (state) this.subscribeToVisibleChannels();
    });

    const offFrame = live.onFrame((frame) => {
      this.lastFrameAt = Date.now();
      for (const [handle, value] of frame.values) {
        this.values[handle] = value;
        this.historyFor(handle).push(frame.t, value);
      }
      for (const [handle, q] of frame.quality) {
        this.quality[handle] = q;
      }
    });

    const offDevice = live.onDevice((handle, state, code) => {
      // A device that fails at run time is not a configuration change, so
      // nothing else would ever refresh it: without this the Hardware page
      // keeps a green RUNNING pill next to a sensor that died an hour ago.
      const device = this.devices.find((d) => d.handle === handle);
      if (!device || device.state === state) return;
      device.state = state;
      device.error =
        state === 'ERROR' || state === 'WARNING'
          ? { code: code ?? 'DEVICE_ERROR', numeric: 0, message: code ?? 'device error' }
          : undefined;
    });

    const offConfig = live.onConfigChanged((revision) => {
      // The firmware bumped its configuration revision — someone (possibly
      // another browser tab) changed the rig.  Re-read the descriptors instead
      // of showing a stale device list.  kUnknownRevision means the firmware
      // said "changed" without saying to what, so re-read unconditionally
      // rather than comparing against a number nobody sent.
      if (revision === kUnknownRevision || revision !== this.configRevision) {
        void this.refresh();
      }
    });

    const offAlert = live.onAlert((severity, code, message) => {
      this.alerts = [
        { id: ++this.alertSeq, severity, code, message, at: Date.now() },
        ...this.alerts,
      ].slice(0, 20);
    });

    live.connect();
    return () => {
      offStatus();
      offFrame();
      offDevice();
      offConfig();
      offAlert();
      live.close();
    };
  }

  dismissAlert(id: number): void {
    this.alerts = this.alerts.filter((a) => a.id !== id);
  }
}

export function describe(error: unknown): string {
  if (error instanceof ApiRequestError) {
    const field = error.field ? ` (${error.field})` : '';
    const detail = error.error.detail ? `: ${error.error.detail}` : '';
    return `${error.error.message}${detail}${field}`;
  }
  if (error instanceof Error) return error.message;
  return String(error);
}

export const controller = new ControllerState();
