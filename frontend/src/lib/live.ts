// =============================================================================
//  live.ts — the single WebSocket connection to /ws/live.
//
//  Design points that matter on an ESP32:
//    * ONE socket for the whole app.  Every extra client costs the firmware a
//      TCP control block and a send buffer.
//    * The firmware pushes batched frames (see docs/websocket.md); this module
//      keeps the last value per channel and notifies subscribers at most once
//      per animation frame, so a 20 Hz stream never triggers 20 Svelte
//      re-render storms per second.
//    * Reconnect with backoff, because a lab controller is expected to survive
//      the operator walking out of Wi-Fi range.
// =============================================================================

import type { ChannelQuality, DeviceState } from './types';

export interface ChannelFrame {
  /** monotonic device time of the batch, ms */
  t: number;
  /** wall-clock ms, 0 when the device has no synchronised time */
  epoch: number;
  /** handle -> processed value */
  values: Map<number, number>;
  /** handle -> quality, only for channels whose quality changed */
  quality: Map<number, ChannelQuality>;
}

type Listener = (frame: ChannelFrame) => void;
type StatusListener = (connected: boolean) => void;
type DeviceListener = (handle: number, state: DeviceState, detail?: string) => void;
type ConfigListener = (revision: number) => void;
type AlertListener = (severity: number, code: string, message: string) => void;
/** M15: a log segment has been closed and is waiting to be collected. */
type LogSegmentListener = (sequence: number) => void;

/** Passed to config listeners when the firmware said "changed" without saying to what. */
export const kUnknownRevision = -1;

const RECONNECT_MIN_MS = 500;
const RECONNECT_MAX_MS = 10_000;

export class LiveConnection {
  private socket: WebSocket | null = null;
  private reconnectDelay = RECONNECT_MIN_MS;
  private frameListeners = new Set<Listener>();
  private statusListeners = new Set<StatusListener>();
  private deviceListeners = new Set<DeviceListener>();
  private configListeners = new Set<ConfigListener>();
  private alertListeners = new Set<AlertListener>();
  private logSegmentListeners = new Set<LogSegmentListener>();
  private pending: ChannelFrame | null = null;
  private rafHandle = 0;
  private closedByUser = false;
  private reconnectTimer = 0;
  private revision = -1;

  /**
   * Which controller is on the other end (M14).  Read from `hello` rather than
   * inferred from the address: in access-point mode every board answers on
   * 192.168.4.1, and a browser filing local recordings by origin would pour two
   * different rigs into one archive.
   */
  controllerId = '';

  readonly latest = new Map<number, number>();

  constructor(private readonly url = defaultUrl()) {}

  connect(): void {
    this.closedByUser = false;
    if (this.reconnectTimer !== 0) {
      clearTimeout(this.reconnectTimer);
      this.reconnectTimer = 0;
    }
    // Calling connect() twice must not leave the first socket open: every extra
    // client costs the firmware a TCP control block and a send buffer.
    if (this.socket !== null) {
      const previous = this.socket;
      previous.onopen = previous.onmessage = previous.onclose = previous.onerror = null;
      previous.close();
    }
    this.socket = new WebSocket(this.url);
    this.socket.binaryType = 'arraybuffer';

    this.socket.onopen = () => {
      this.reconnectDelay = RECONNECT_MIN_MS;
      this.statusListeners.forEach((fn) => fn(true));
    };

    this.socket.onmessage = (event) => this.handleMessage(event.data);

    this.socket.onclose = () => {
      this.statusListeners.forEach((fn) => fn(false));
      if (this.closedByUser) return;
      this.reconnectTimer = window.setTimeout(() => {
        this.reconnectTimer = 0;
        this.connect();
      }, this.reconnectDelay);
      this.reconnectDelay = Math.min(this.reconnectDelay * 2, RECONNECT_MAX_MS);
    };

    // onerror is always followed by onclose; nothing extra to do here.
    this.socket.onerror = () => this.socket?.close();
  }

  close(): void {
    this.closedByUser = true;
    // A reconnect scheduled a moment ago would otherwise fire after teardown
    // and — because connect() clears closedByUser — resurrect a socket nobody
    // is listening to, which then reconnects forever.
    if (this.reconnectTimer !== 0) {
      clearTimeout(this.reconnectTimer);
      this.reconnectTimer = 0;
    }
    // Cancel a frame that is already queued: delivering it after the view has
    // been torn down would update components that no longer exist.
    if (this.rafHandle !== 0) {
      cancelAnimationFrame(this.rafHandle);
      this.rafHandle = 0;
      this.pending = null;
    }
    this.socket?.close();
  }

  onFrame(fn: Listener): () => void {
    this.frameListeners.add(fn);
    return () => this.frameListeners.delete(fn);
  }

  onStatus(fn: StatusListener): () => void {
    this.statusListeners.add(fn);
    return () => this.statusListeners.delete(fn);
  }

  onDevice(fn: DeviceListener): () => void {
    this.deviceListeners.add(fn);
    return () => this.deviceListeners.delete(fn);
  }

  /**
   * Fires on `hello` with the firmware's config_revision, and on every `config`
   * message with `kUnknownRevision` — meaning "something changed, re-read".
   */
  onConfigChanged(fn: ConfigListener): () => void {
    this.configListeners.add(fn);
    return () => this.configListeners.delete(fn);
  }

  onAlert(fn: AlertListener): () => void {
    this.alertListeners.add(fn);
    return () => this.alertListeners.delete(fn);
  }

  /**
   * A segment is ready for collection (M15 §15).
   *
   * Purely an accelerator.  The collector re-reads the queue over REST on a
   * timer regardless, so a dropped frame delays a transfer and can never lose
   * one — which is why nothing here needs delivery guarantees.
   */
  onLogSegmentReady(fn: LogSegmentListener): () => void {
    this.logSegmentListeners.add(fn);
    return () => this.logSegmentListeners.delete(fn);
  }

  get configRevision(): number {
    return this.revision;
  }

  /** Subscribes to a channel subset; the firmware only streams what is asked for. */
  subscribe(handles: number[]): void {
    this.send({ type: 'subscribe', channels: handles });
  }

  private send(payload: unknown): void {
    if (this.socket?.readyState === WebSocket.OPEN) {
      this.socket.send(JSON.stringify(payload));
    }
  }

  private handleMessage(data: string | ArrayBuffer): void {
    if (typeof data !== 'string') return; // binary frames land in Milestone 6
    const message = JSON.parse(data) as Record<string, unknown>;

    switch (message.type) {
      case 'channels': {
        const values = message.data as Record<string, number>;
        const quality = (message.quality ?? {}) as Record<string, ChannelQuality>;
        const frame: ChannelFrame = {
          t: Number(message.t ?? 0),
          epoch: Number(message.epoch ?? 0),
          values: new Map(),
          quality: new Map(),
        };
        for (const [handle, value] of Object.entries(values)) {
          const key = Number(handle);
          frame.values.set(key, value);
          this.latest.set(key, value);
        }
        for (const [handle, q] of Object.entries(quality)) {
          frame.quality.set(Number(handle), q);
        }
        this.schedule(frame);
        break;
      }
      case 'device': {
        // The firmware sends `state` (the state name) and `code` (the error
        // symbol); there is no free-text detail on this message.
        const code = String(message.code ?? '');
        this.deviceListeners.forEach((fn) =>
          fn(Number(message.handle), message.state as DeviceState,
             code && code !== 'OK' ? code : undefined));
        break;
      }
      case 'hello': {
        this.revision = Number(message.config_revision ?? -1);
        if (typeof message.controller_id === 'string' && message.controller_id) {
          this.controllerId = message.controller_id;
        }
        this.configListeners.forEach((fn) => fn(this.revision));
        break;
      }
      case 'config': {
        // The firmware does not send the new configuration over the socket —
        // it says "something changed" and the client re-reads the descriptors.
        // One source of truth, no duplicated state in the telemetry stream.
        //
        // It does not send the new revision either, so the client must NOT
        // guess one: incrementing a local counter drifts the moment two
        // changes are coalesced or a frame is dropped, and once the guess
        // catches up with the firmware's number the "did it change?" test
        // stops firing for good.  kUnknownRevision means "re-read now".
        this.configListeners.forEach((fn) => fn(kUnknownRevision));
        break;
      }
      case 'log_segment': {
        // No session id: the event says only "a part closed".  The collector
        // knows which session it owns, and re-reads the queue to find out what.
        this.logSegmentListeners.forEach((fn) => fn(Number(message.sequence ?? 0)));
        break;
      }
      case 'alarm':
      case 'system': {
        this.alertListeners.forEach((fn) =>
          fn(Number(message.severity ?? 2), String(message.code ?? ''),
             String(message.message ?? '')));
        break;
      }
      default:
        // Unknown message types are ignored on purpose: the firmware may be
        // newer than the cached SPA after an OTA update.
        break;
    }
  }

  /** Coalesces bursts into one repaint per frame. */
  private schedule(frame: ChannelFrame): void {
    if (this.pending) {
      frame.values.forEach((v, k) => this.pending!.values.set(k, v));
      frame.quality.forEach((v, k) => this.pending!.quality.set(k, v));
      this.pending.t = frame.t;
      this.pending.epoch = frame.epoch;
      return;
    }
    this.pending = frame;
    this.rafHandle = requestAnimationFrame(() => {
      const toDeliver = this.pending!;
      this.pending = null;
      this.rafHandle = 0;
      this.frameListeners.forEach((fn) => fn(toDeliver));
    });
  }
}

function defaultUrl(): string {
  const protocol = location.protocol === 'https:' ? 'wss:' : 'ws:';
  return `${protocol}//${location.host}/ws/live`;
}

export const live = new LiveConnection();
