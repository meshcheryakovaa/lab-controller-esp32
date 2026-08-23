// =============================================================================
//  api.ts — the typed REST client.
//
//  Two things it exists to guarantee:
//    * every non-2xx response is turned into an ApiRequestError carrying the
//      firmware's `code` and `field`, so a form can highlight the right input
//      instead of showing "request failed";
//    * `validateDevice()` and `createDevice()` hit the SAME endpoint, so what
//      the form validates is exactly what the create will do.
// =============================================================================

import type { SegmentQueue } from './log-offload/SegmentCollector';
import type {
  ApiError, AuthStatus, Calibration, CalibrationFit, CalibrationKind,
  CalibrationResidual,
  Channel, ControlDocument, ControlLoop, Device, Experiment, ExperimentSummary,
  LogEntry, LoggingStatus, LoopMode, ModuleManifest, OutputState, RunRecord,
  RunStatus, SafetyLimit,
  NetworkStatus, NetworkScan, NetworkState, ScanState,
  CloudStatus, CloudQueue, CloudLinkPrompt, CloudLinkState,
} from './types';
import type { Dashboard, DashboardSummary } from './widgets';

export class ApiRequestError extends Error {
  constructor(
    readonly status: number,
    readonly error: ApiError,
  ) {
    super(error.message || error.code);
    this.name = 'ApiRequestError';
  }

  /** The configuration key the firmware objected to, if any. */
  get field(): string | undefined {
    return this.error.field;
  }
}

const BASE = '/api/v1';

async function request<T>(
  method: string,
  path: string,
  body?: unknown,
  query?: Record<string, string | number | boolean>,
): Promise<T> {
  const url = new URL(BASE + path, location.origin);
  for (const [key, value] of Object.entries(query ?? {})) {
    url.searchParams.set(key, String(value));
  }

  const response = await fetch(url, {
    method,
    headers: body !== undefined ? { 'Content-Type': 'application/json' } : undefined,
    body: body !== undefined ? JSON.stringify(body) : undefined,
    // The session is an HttpOnly cookie: the page cannot read it, which is the
    // point, so it has to be sent rather than attached (ADR-0020).
    credentials: 'same-origin',
  });

  const text = await response.text();
  // A body that is not JSON must not destroy the status code: a captive portal
  // or a plain-text 500 from the HTTP stack would otherwise surface as
  // "Unexpected token '<'" and lose both the status and the offending field.
  let payload: any = {};
  if (text) {
    try {
      payload = JSON.parse(text);
    } catch {
      if (response.ok) {
        throw new ApiRequestError(response.status, {
          code: 'BAD_RESPONSE',
          numeric: 0,
          message: 'the controller answered with something that is not JSON',
          detail: text.slice(0, 120),
        });
      }
    }
  }

  if (!response.ok) {
    // The firmware always answers with the same envelope, so there is exactly
    // one place in the whole frontend that needs to know its shape.
    const error: ApiError = payload.error ?? {
      code: 'UNKNOWN',
      numeric: 0,
      message: `HTTP ${response.status}`,
    };
    throw new ApiRequestError(response.status, error);
  }
  return payload as T;
}

// --- pin picker --------------------------------------------------------------
export interface GpioPin {
  pin: number;
  usable: boolean;
  input_only?: boolean;
  strapping?: boolean;
  adc1?: number;
  adc2?: number;
  reason?: string;
  advisory?: string;
  owner?: string;
  owner_device?: number;
  use?: string;
}

export interface GpioMap {
  chip: string;
  i2c_buses: number;
  pwm_channels: number;
  pins: GpioPin[];
}

export interface I2cScanResult {
  bus: number;
  found: Array<{
    address: string;
    address_decimal: number;
    claimed_by?: string;
    candidates: Array<{ module: string; label: string; confidence: 'likely' | 'possible' }>;
  }>;
}

export const api = {
  system: () => request<Record<string, unknown>>('GET', '/system'),
  diagnostics: () => request<Record<string, unknown>>('GET', '/diagnostics'),
  reboot: () => request<{ rebooting: boolean }>('POST', '/system/reboot', {}),

  /** The catalogue the "Add device" form is generated from. */
  modules: () => request<{ modules: ModuleManifest[] }>('GET', '/modules'),
  module: (id: string) => request<ModuleManifest>('GET', `/modules/${id}`),

  /** Capability and ownership per pin — the frontend never re-derives this. */
  gpio: () => request<GpioMap>('GET', '/gpio'),
  scanI2c: (bus: number) => request<I2cScanResult>('POST', `/buses/i2c/${bus}/scan`, {}),

  devices: () => request<{ devices: Device[] }>('GET', '/devices'),
  device: (key: string) => request<Device>('GET', `/devices/${encodeURIComponent(key)}`),

  /**
   * Live form validation.  Same endpoint, same code path, no side effects —
   * which is why its verdict can be trusted.
   */
  validateDevice: (entry: unknown) =>
    request<{ valid: boolean; key: string }>('POST', '/devices', entry, { dry_run: 1 }),

  createDevice: (entry: unknown) => request<Device>('POST', '/devices', entry),
  patchDevice: (key: string, patch: unknown) =>
    request<Device>('PATCH', `/devices/${encodeURIComponent(key)}`, patch),
  deleteDevice: (key: string) =>
    request<{ deleted: string }>('DELETE', `/devices/${encodeURIComponent(key)}`),
  deviceAction: (key: string, action: string) =>
    request<Record<string, unknown>>(
      'POST', `/devices/${encodeURIComponent(key)}/actions/${action}`, {}),

  channels: (withValues = false) =>
    request<{ channels: Channel[] }>('GET', '/channels',
      undefined, withValues ? { values: 1 } : undefined),
  channel: (key: string) => request<Channel>('GET', `/channels/${encodeURIComponent(key)}`),
  writeChannel: (key: string, value: number) =>
    request<Channel>('POST', `/channels/${encodeURIComponent(key)}/write`, { value }),

  processing: (channelKey: string) =>
    request<Record<string, unknown>>('GET', `/processing/${encodeURIComponent(channelKey)}`),
  setProcessing: (channelKey: string, pipeline: unknown) =>
    request<{ active_stages: string[] }>(
      'PUT', `/processing/${encodeURIComponent(channelKey)}`, pipeline),

  // --- calibration (Milestone 5) --------------------------------------------
  /** Fits and reports. Stores nothing — a preview that saves is not a preview. */
  solveCalibration: (draft: unknown) =>
    request<{ kind: CalibrationKind; fit: CalibrationFit; residuals: CalibrationResidual[] }>(
      'POST', '/calibrations/solve', draft),
  calibrations: (channelKey?: string) =>
    request<{ calibrations: Calibration[] }>(
      'GET', '/calibrations', undefined,
      channelKey ? { channel: channelKey } : undefined),
  createCalibration: (draft: unknown) =>
    request<{ id: string; channel: string; version: number; active: boolean;
              fit: CalibrationFit; residuals: CalibrationResidual[] }>(
      'POST', '/calibrations', draft),
  activateCalibration: (id: string) =>
    request<{ id: string; channel: string; active: boolean }>(
      'POST', `/calibrations/${encodeURIComponent(id)}/activate`, {}),
  deactivateCalibration: (id: string) =>
    request<{ id: string; channel: string; active: boolean }>(
      'POST', `/calibrations/${encodeURIComponent(id)}/deactivate`, {}),
  deleteCalibration: (id: string) =>
    request<{ deleted: string }>('DELETE', `/calibrations/${encodeURIComponent(id)}`),

  // --- outputs (Milestone 7) ------------------------------------------------
  outputs: () =>
    request<{ tripped: boolean; reason: string; expiries: number; trips: number;
              outputs: Array<{ channel: string; unit: string; handle: number;
                               output: OutputState }> }>('GET', '/outputs'),
  /** "Still here" — renews the deadline without changing the value. */
  renewOutput: (channelKey: string) =>
    request<{ output: OutputState }>(
      'POST', `/outputs/${encodeURIComponent(channelKey)}/renew`, {}),
  releaseOutput: (channelKey: string) =>
    request<{ output: OutputState }>(
      'POST', `/outputs/${encodeURIComponent(channelKey)}/release`, {}),
  /** The master stop. Everything to its safe state, and nothing commandable. */
  tripOutputs: () =>
    request<{ tripped: boolean; reason: string }>('POST', '/outputs/trip', {}),
  clearOutputTrip: () =>
    request<{ tripped: boolean; reason: string }>('POST', '/outputs/clear', {}),

  // --- control (Milestone 8) ------------------------------------------------
  /** Loops, rules and limits, with their live state. */
  control: () => request<ControlDocument>('GET', '/control'),
  /**
   * Replaces the whole document.  Whole, not patched: a control configuration
   * applied half-way is a rig with some of its interlocks, and the firmware
   * validates every entry before installing any of them.
   */
  saveControl: (document: { loops?: unknown[]; rules?: unknown[];
                            limits?: unknown[]; password?: string }) =>
    request<ControlDocument & { applied: { ok: number; failed: number } }>(
      'PUT', '/control', document),
  validateControl: (document: unknown) =>
    request<{ dry_run: boolean; valid: boolean }>('PUT', '/control', document, { dry_run: 1 }),

  /** Runtime, and deliberately not persisted: every loop reboots into OFF. */
  setLoopMode: (id: string, mode: LoopMode, value?: number) =>
    request<ControlLoop>('POST', `/control/loops/${encodeURIComponent(id)}/mode`,
      value === undefined ? { mode } : { mode, value }),
  /** The number persists — the authority to act on it does not. */
  setSetpoint: (id: string, value: number) =>
    request<ControlLoop>('POST', `/control/loops/${encodeURIComponent(id)}/setpoint`,
      { value }),
  setManualValue: (id: string, value: number) =>
    request<ControlLoop>('POST', `/control/loops/${encodeURIComponent(id)}/manual`,
      { value }),

  /** Re-arms one interlock. If its cause is still there it trips again at once. */
  resetLimit: (id: string) =>
    request<SafetyLimit>('POST', `/control/limits/${encodeURIComponent(id)}/reset`, {}),
  resetAllLimits: () => request<ControlDocument>('POST', '/control/limits/reset', {}),

  // --- experiments (Milestone 9) --------------------------------------------
  experiments: () =>
    request<{ experiments: ExperimentSummary[]; run: RunStatus;
              limits: { steps: number; events: number; records: number } }>(
      'GET', '/experiments'),
  experiment: (key: string) =>
    request<Experiment>('GET', `/experiments/${encodeURIComponent(key)}`),
  /** Same endpoint validates and saves, so the editor's verdict is the real one. */
  validateExperiment: (experiment: Experiment) =>
    request<{ valid: boolean; steps: number; runnable: boolean;
              blocking_step?: number; blocking_reason?: string }>(
      'POST', '/experiments', experiment, { dry_run: 1 }),
  createExperiment: (experiment: Experiment) =>
    request<{ key: string; steps: number; runnable: boolean;
              blocking_step?: number; blocking_reason?: string }>(
      'POST', '/experiments', experiment),
  saveExperiment: (key: string, experiment: Experiment) =>
    request<{ key: string; steps: number; runnable: boolean;
              blocking_step?: number; blocking_reason?: string }>(
      'PUT', `/experiments/${encodeURIComponent(key)}`, experiment),
  deleteExperiment: (key: string) =>
    request<{ deleted: string }>('DELETE', `/experiments/${encodeURIComponent(key)}`),

  /** The live run. Polled while anything is watching it. */
  runState: () => request<RunStatus>('GET', '/experiments/state'),
  /** What this rig has actually done, whether or not it went well. */
  runs: () => request<{ runs: RunRecord[] }>('GET', '/experiments/runs'),

  /** Metadata travels with the RUN, not with the scenario: the operator and the
      sample are properties of this run and are kept forever (§48). */
  startExperiment: (key: string, metadata: { operator: string; sample?: string; notes?: string }) =>
    request<RunStatus>(
      'POST', `/experiments/${encodeURIComponent(key)}/actions/start`, metadata),
  pauseExperiment: (key: string) =>
    request<RunStatus>('POST', `/experiments/${encodeURIComponent(key)}/actions/pause`, {}),
  resumeExperiment: (key: string) =>
    request<RunStatus>('POST', `/experiments/${encodeURIComponent(key)}/actions/resume`, {}),
  stopExperiment: (key: string) =>
    request<RunStatus>('POST', `/experiments/${encodeURIComponent(key)}/actions/stop`, {}),

  // --- access (Milestone 11) ------------------------------------------------
  auth: () => request<AuthStatus>('GET', '/auth'),
  login: (password: string) =>
    request<{ signed_in: boolean }>('POST', '/auth/login', { password }),
  logout: () => request<{ signed_in: boolean }>('POST', '/auth/logout', {}),
  /** Changing the credential ends every session, including this one. */
  setPassword: (password: string, current?: string) =>
    request<{ configured: boolean }>('POST', '/auth/password',
      current ? { password, current } : { password }),

  /** Refused while anything is running, and confirmed by the password even
      then — authorisation is not the only question an OTA has to answer. */
  otaCheck: (password: string) =>
    request<Record<string, unknown>>('POST', '/firmware/ota', { password }),
  /** The configuration as it was before the last import. */
  configBackup: () => request<Record<string, unknown>>('GET', '/config/backup'),

  // --- logging (Milestone 10) -----------------------------------------------
  logs: () =>
    request<{ logs: LogEntry[]; recording: LoggingStatus;
              limits: { sessions: number; channels: number; rate_hz: number } }>(
      'GET', '/logs'),
  log: (id: string) => request<LogEntry>('GET', `/logs/${encodeURIComponent(id)}`),
  startLog: (spec: { name: string; operator?: string; sample?: string;
                     rate_hz: number; raw?: boolean; channels: string[];
                     expected_s?: number;
                     // M15.  Absent means `single` — the mode every session had
                     // before, and the one an old client still gets.
                     storage_mode?: 'single' | 'continuous_offload';
                     segment_bytes?: number;
                     collector_id?: string }) =>
    request<LoggingStatus>('POST', '/logs/start', spec),
  stopLog: () => request<LoggingStatus>('POST', '/logs/stop', {}),
  deleteLog: (id: string) =>
    request<{ deleted: string }>('DELETE', `/logs/${encodeURIComponent(id)}`),
  /** Not fetched: handed to the browser, which streams it to disk.  A dataset
      is megabytes and does not belong in a JavaScript string (ADR-0019). */
  logDownloadUrl: (id: string) => `${BASE}/logs/${encodeURIComponent(id)}/export.csv`,

  // --- the offload queue (Milestone 15) -------------------------------------
  /** What is waiting on the controller.  REST is the source of truth here: a
      collector that missed a WebSocket notification rebuilds its whole to-do
      list from this, so a lost frame can never become a lost segment. */
  logSegments: (id: string) =>
    request<SegmentQueue>('GET', `/logs/${encodeURIComponent(id)}/segments`),

  /** One segment, as bytes.  Not `request()`: this is a file, and running it
      through JSON parsing would both corrupt it and hold it in a string. */
  downloadLogSegment: async (id: string, sequence: number): Promise<Uint8Array> => {
    const response = await fetch(
      `${BASE}/logs/${encodeURIComponent(id)}/segments/${sequence}/export.csv`,
      { credentials: 'same-origin', cache: 'no-store' });
    if (!response.ok) {
      throw new Error(`segment ${sequence}: HTTP ${response.status}`);
    }
    return new Uint8Array(await response.arrayBuffer());
  },

  /** The call that deletes a CSV from the controller.  Sent only after the
      segment is verified AND committed to this device's archive. */
  ackLogSegment: (id: string, sequence: number,
                  proof: { collector_id: string; bytes: number;
                           payload_crc32: string }) =>
    request<{ acknowledged: boolean; deleted: boolean; sequence: number;
              already_acknowledged?: boolean; pending_segments: number }>(
      'POST', `/logs/${encodeURIComponent(id)}/segments/${sequence}/ack`, proof),

  // --- dashboards (Milestone 6) ---------------------------------------------
  /** Summaries only — eight full layouts do not fit in one response. */
  dashboards: () =>
    request<{ dashboards: DashboardSummary[];
              limits: { dashboards: number; widgets: number; columns: number } }>(
      'GET', '/dashboards'),
  dashboard: (key: string) =>
    request<Dashboard & { health?: { widgets: number; dangling_channels: number } }>(
      'GET', `/dashboards/${encodeURIComponent(key)}`),
  createDashboard: (dashboard: Dashboard) =>
    request<{ key: string; health: { dangling_channels: number } }>(
      'POST', '/dashboards', dashboard),
  saveDashboard: (key: string, dashboard: Dashboard) =>
    request<{ key: string; health: { dangling_channels: number } }>(
      'PUT', `/dashboards/${encodeURIComponent(key)}`, dashboard),
  deleteDashboard: (key: string) =>
    request<{ deleted: string }>('DELETE', `/dashboards/${encodeURIComponent(key)}`),

  exportConfig: () => request<Record<string, unknown>>('GET', '/config/export'),
  /** Confirmed by the password: an import replaces every section, interlocks
      included, on a rig that may be running. */
  importConfig: (document: Record<string, unknown>, password?: string) =>
    request<{ sections_written: number; devices_started: number;
              devices_failed: number; backup_saved?: boolean }>(
      'POST', '/config/import',
      password ? { ...document, password } : document),

  // --- M16: the house network ---------------------------------------------
  network: () => request<NetworkStatus>('GET', '/network'),

  startNetworkScan: () => request<{ state: ScanState }>('POST', '/network/scan', {}),
  networkScan: () => request<NetworkScan>('GET', '/network/scan'),

  /** Asks the controller to PROVE these credentials.  Answers 202 as soon as
   *  the attempt is accepted — the caller polls network() for the outcome.
   *  Anything else would mean holding a request open for the fifteen seconds a
   *  join can take, on the same server the poll has to reach. */
  connectNetwork: (ssid: string, password: string) =>
    request<{ accepted: boolean; state: NetworkState }>(
      'POST', '/network/connect', { ssid, password }),

  forgetNetwork: () =>
    request<{ cleared: boolean; state: NetworkState; ip: string; ssid: string }>(
      'DELETE', '/network/config'),

  setHostname: (hostname: string) =>
    request<{ hostname: string }>('PUT', '/network/hostname', { hostname }),

  // --- M17: offloading to Yandex Disk --------------------------------------
  cloud: () => request<CloudStatus>('GET', '/cloud'),
  cloudQueue: () => request<CloudQueue>('GET', '/cloud/queue'),

  /** An empty clientSecret means "leave the stored one alone" — the page cannot
   *  show it, so it must not force a re-type to change something else. */
  saveCloudConfig: (config: {
    clientId?: string; clientSecret?: string; clearClientSecret?: boolean;
    rootPath?: string; enabled?: boolean;
  }) => request<CloudStatus>('PUT', '/cloud/yandex/config', config),

  /** Starts Device Code.  Returns the code to type; the controller does the
   *  polling of Yandex itself, so this works with the browser closed. */
  beginCloudLink: () =>
    request<CloudLinkPrompt>('POST', '/cloud/yandex/device-code', {}),
  cloudLinkStatus: () =>
    request<{ state: CloudLinkState; expiresIn: number }>(
      'GET', '/cloud/yandex/device-code/status'),

  testCloudAccess: () => request<{ ok: boolean }>('POST', '/cloud/yandex/test', {}),

  disconnectCloud: (password: string) =>
    request<{ disconnected: boolean; revokedRemotely: boolean; note: string }>(
      'DELETE', '/cloud/yandex/credentials', { password }),

  pauseCloudQueue: (paused: boolean) =>
    request<{ paused: boolean }>(
      'POST', `/cloud/queue/${paused ? 'pause' : 'resume'}`, {}),

  /** By id only.  A client never names a path or an upload URL. */
  retryCloudJob: (jobId: number) =>
    request<{ retrying: number }>('POST', '/cloud/queue/retry', { jobId }),
};
