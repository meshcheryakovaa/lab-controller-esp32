// =============================================================================
//  Mirror of the firmware's public data model.
//
//  These types are hand-written but must match core/ModuleManifest.h and
//  services/ChannelManager.h exactly.  docs/api.md is the contract; when it
//  changes, both sides change together in the same commit.
// =============================================================================

export type ModuleCategory =
  | 'sensor' | 'output' | 'processing' | 'control' | 'virtual' | 'system';

export type ParamType =
  | 'gpio' | 'int' | 'float' | 'bool' | 'select' | 'text'
  | 'i2c_address' | 'bus_ref' | 'channel_ref';

export interface ParamSpec {
  key: string;
  label: string;
  type: ParamType;
  unit?: string;
  help?: string;
  min?: number;
  max?: number;
  step?: number;
  default?: string | number | boolean;
  options?: Array<{ value: string; label: string }>;
  pin_use?: 'digital_input' | 'digital_output' | 'analog_input' | 'pwm_output' | 'bus_signal';
  required: boolean;
  advanced: boolean;
  visible_if?: string;
}

export interface ChannelSpec {
  id: string;
  name: string;
  unit: string;
  quantity: string;
  direction: 'input' | 'output';
  min?: number;
  max?: number;
  precision: number;
}

export interface ModuleManifest {
  id: string;
  name: string;
  category: ModuleCategory;
  description?: string;
  bus: 'none' | 'i2c' | 'spi' | 'uart' | 'onewire';
  params: ParamSpec[];
  channels: ChannelSpec[];
  max_instances: number;
  default_sample_interval_us: number;
  min_sample_interval_us: number;
  schema_version: number;
}

export type DeviceState =
  | 'DISABLED' | 'CONFIGURED' | 'INITIALIZING' | 'RUNNING' | 'WARNING' | 'ERROR';

export type ChannelQuality =
  | 'UNKNOWN' | 'GOOD' | 'STALE' | 'OUT_OF_RANGE' | 'SATURATED' | 'FAULTED';

export interface Geometry {
  system: 'none' | 'cartesian' | 'cylindrical';
  a: number;
  b: number;
  c: number;
  group?: string;
  role?: string;
}

export interface ChannelReading {
  raw: number;
  calibrated: number;
  processed: number;
  quality: ChannelQuality;
  sequence: number;
  t: number;
  epoch: number;
}

// --- calibration (Milestone 5) ----------------------------------------------
export type CalibrationKind = 'offset' | 'linear' | 'poly2' | 'poly3' | 'table';

export interface CalibrationPoint {
  raw: number;
  reference: number;
}

export interface CalibrationFit {
  coefficients: number[];
  order: number;
  x_center: number;
  x_scale: number;
  /** Root-mean-square residual, in the calibrated unit. */
  rms_residual: number;
  max_residual: number;
  r_squared: number;
}

export interface CalibrationResidual extends CalibrationPoint {
  predicted: number;
  residual: number;
}

/**
 * One version.  Records are immutable: recalibrating appends a new one, so a
 * dataset recorded last week can always be traced to the numbers behind it.
 */
export interface Calibration {
  id: string;
  channel: string;
  version: number;
  kind: CalibrationKind;
  unit?: string;
  precision?: number;
  min?: number;
  max?: number;
  note?: string;
  created_epoch_ms: number;
  active: boolean;
  points: CalibrationPoint[];
  fit: CalibrationFit;
}

/**
 * The safety state of an output channel (§27, ADR-0016).  Present on every
 * channel that is an output, everywhere a channel is served — there is no
 * separate screen on which an actuator looks unattended.
 */
export interface OutputState {
  state: 'SAFE' | 'COMMANDED' | 'EXPIRED' | 'DEVICE_FAULT' | 'TRIPPED';
  safe_value: number;
  commanded: number;
  applied: number;
  /** Seconds a command stays valid without being renewed; 0 waives it. */
  hold_s: number;
  /** Present only while COMMANDED with a deadline. */
  expires_in_s?: number;
}

export interface Channel {
  handle: number;
  key: string;
  name: string;
  unit: string;
  quantity: string;
  source: number;
  direction: 'input' | 'output';
  min: number;
  max: number;
  precision: number;
  color: string;
  logged: boolean;
  visible: boolean;
  geometry?: Geometry;
  /** Present only when the list was requested with ?values=1. */
  value?: ChannelReading;
  /** Present only on output channels. */
  output?: OutputState;
}

/**
 * A device's channel as it appears inside a device descriptor — a summary, not
 * the full `Channel`.  The firmware sends objects here, not bare handles.
 */
export interface DeviceChannelRef {
  handle: number;
  key: string;
  unit: string;
  stages: number;
}

export interface Device {
  handle: number;
  key: string;
  name: string;
  module: string;
  state: DeviceState;
  error?: ApiError;
  sample_interval_us: number;
  channels: DeviceChannelRef[];
  geometry?: Geometry;
  /** Present only on `GET /devices/{key}`; the list endpoint omits it. */
  config?: Record<string, unknown>;
}

/** Uniform error envelope — every non-2xx response has exactly this shape. */
export interface ApiError {
  code: string;      // "GPIO_RESERVED"
  numeric: number;   // 203
  message: string;   // human sentence
  detail?: string;   // "used by I2C0 SDA"
  field?: string;    // offending configuration key, when applicable
}

// --- control (Milestone 8) ---------------------------------------------------
//  Three lists in one document, and they are not interchangeable: a limit is
//  enforced independently of every regulator, a loop regulates, and a rule is
//  convenience automation that is explicitly NOT a safety mechanism (§30).

export type LoopMode = 'off' | 'manual' | 'automatic';
export type LoopState = 'IDLE' | 'RUNNING' | 'NO_INPUT' | 'BLOCKED';

export interface ControlLoop {
  id: string;
  input: string;
  output: string;
  setpoint: number;
  kp: number;
  ki: number;
  kd: number;
  min: number;
  max: number;
  manual: number;
  invert: boolean;
  period_s: number;
  input_grace_s: number;

  // --- live, present on GET only --------------------------------------------
  mode?: LoopMode;
  state?: LoopState;
  /** What the PID asked for. */
  output_value?: number;
  /** What the actuator is doing — different while blocked or power-limited. */
  output_applied?: number;
  last_error?: number;
  integral?: number;
  measured?: number;
  quality?: string;
  unit?: string;
  input_present?: boolean;
  output_present?: boolean;
  fault?: { code: string; numeric: number; detail?: string };
}

export interface ControlRule {
  id: string;
  input: string;
  output: string;
  on_above: number;
  off_below: number;
  on_value: number;
  off_value: number;
  min_hold_s: number;
  enabled: boolean;
  note?: string;

  engaged?: boolean;
  holding?: boolean;
  activations?: number;
  input_present?: boolean;
  output_present?: boolean;
}

export type SafetyCondition = 'above' | 'below' | 'outside';
export type SafetyAction = 'trip_all' | 'release_output' | 'alarm_only';

export interface SafetyLimit {
  id: string;
  channel: string;
  target?: string;
  condition: SafetyCondition;
  action: SafetyAction;
  low: number;
  high: number;
  for_s: number;
  require_fresh_input: boolean;
  enabled: boolean;
  message?: string;

  violating?: boolean;
  latched?: boolean;
  trips?: number;
  channel_present?: boolean;
  fault?: { code: string; numeric: number; detail?: string };
}

export interface ControlDocument {
  loops: ControlLoop[];
  rules: ControlRule[];
  limits: SafetyLimit[];
  latched?: number;
  tripped?: boolean;
  trip_reason?: string;
  limits_max?: { loops: number; rules: number; limits: number };
}

// --- experiments (Milestone 9) -----------------------------------------------
//  The vocabulary mirrors the firmware's, and that is the point: a step is an
//  object with an `op` from a closed list, never a string of code (ADR-0018).

export type StepOp =
  | 'SET' | 'WAIT' | 'WAIT_UNTIL' | 'RUN_FOR' | 'MARK_EVENT'
  | 'ENABLE' | 'DISABLE' | 'STOP';

export type Comparison = '>=' | '<=' | '>' | '<';

export interface ExperimentStep {
  op: StepOp;
  target?: string;
  value?: number;
  mode?: string;
  channel?: string;
  comparison?: Comparison;
  timeout_s?: number;
  on_timeout?: 'abort' | 'continue';
  duration_s?: number;
  label?: string;
}

export interface ExperimentMetadata {
  operator?: string;
  sample?: string;
  description?: string;
  notes?: string;
}

/** What a scenario records.  A property of the scenario, not of a step: the
    steps say WHEN, this says WHAT (ADR-0019). */
export interface ExperimentLogging {
  channels: string[];
  rate_hz?: number;
  raw?: boolean;
}

export interface Experiment {
  key: string;
  name: string;
  metadata?: ExperimentMetadata;
  logging?: ExperimentLogging;
  steps: ExperimentStep[];
}

export interface ExperimentSummary {
  key: string;
  name: string;
  steps: number;
  description?: string;
}

export type RunState = 'IDLE' | 'RUNNING' | 'PAUSED' | 'FINISHED' | 'ABORTED';

export interface RunEvent {
  at_s: number;
  step: number;
  label: string;
}

export interface RunStatus {
  state: RunState;
  experiment: string;
  name: string;
  step: number;
  steps: number;
  operator?: string;
  sample?: string;
  current?: {
    op: StepOp;
    target?: string;
    channel?: string;
    label?: string;
    remaining_s?: number;
  };
  reason?: string;
  step_reached?: number;
  error?: ApiError;
  events: RunEvent[];
  events_dropped?: number;
  record_pending?: boolean;
}

/** One line of history. The three fields that matter are state, reason and
    step_reached: an aborted run must never read as a finished one. */
export interface RunRecord {
  experiment: string;
  name: string;
  operator?: string;
  sample?: string;
  notes?: string;
  started_epoch_ms: number;
  ended_epoch_ms: number;
  duration_s: number;
  state: RunState;
  reason: string;
  step_reached: number;
  steps: number;
  error?: { code: string; numeric: number; detail?: string };
  config_revision?: number;
  firmware?: string;
  devices?: string[];
  calibrations?: string[];
  events?: RunEvent[];
  events_dropped?: number;
}

// --- logging (Milestone 10) --------------------------------------------------

export interface LogEntry {
  id: string;
  name: string;
  experiment?: string;
  operator?: string;
  sample?: string;
  path: string;
  rate_hz: number;
  channels: number;
  columns?: string[];
  state: 'RECORDING' | 'COMPLETE' | 'TRUNCATED';
  reason?: string;
  rows: number;
  dropped: number;
  bytes: number;
  /** The three fields that keep an incomplete dataset from reading as a whole
      one; they travel together in the index, the CSV footer and the UI. */
  truncated: boolean;
  started_epoch_ms?: number;
  duration_s?: number;
  config_revision?: number;
  config_fingerprint?: number;
  firmware?: string;
  error?: { code: string; detail?: string };
}

export interface LoggingStatus {
  recording: boolean;
  id: string;
  name: string;
  rows: number;
  dropped_rows: number;
  bytes: number;
  rate_hz: number;
  channels: number;
  writable_bytes?: number;
  reserve_bytes?: number;
  last_stop?: string;
  last_truncated?: boolean;
  last_error?: string;
}

// --- access (Milestone 11) ---------------------------------------------------

export interface AuthStatus {
  configured: boolean;
  signed_in: boolean;
  sessions: number;
  locked: boolean;
  min_password_length?: number;
}
