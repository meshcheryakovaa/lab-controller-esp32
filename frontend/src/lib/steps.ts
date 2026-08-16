// =============================================================================
//  steps.ts — THE step vocabulary, for the editor (§32, ADR-0018).
//
//  This file is to the scenario editor what widgets.ts is to the dashboard and
//  BuiltinModules.cpp is to the firmware: one list, and the only place that has
//  to change when a step type is added.  The firmware has the same list and
//  refuses anything outside it, so this is a form generator, not a validator.
//
//  There is deliberately no "advanced" step, no expression field and no escape
//  hatch: a scenario is data.  The moment the editor grows a text box that the
//  firmware evaluates, every promise in the security section is gone.
// =============================================================================

import type { ExperimentStep, StepOp } from './types';

export type StepFieldKind =
  | 'target'      // an output channel, a loop, or "<loop>.setpoint"
  | 'loopTarget'  // a loop or a device, for ENABLE / DISABLE
  | 'channel'
  | 'comparison'
  | 'number'
  | 'seconds'
  | 'text'
  | 'onTimeout'
  | 'mode';

export interface StepField {
  key: keyof ExperimentStep;
  label: string;
  kind: StepFieldKind;
  help?: string;
}

export interface StepType {
  op: StepOp;
  name: string;
  description: string;
  fields: StepField[];
  defaults: () => Partial<ExperimentStep>;
  /** One line, for the step list — what this step will actually do. */
  summary: (step: ExperimentStep) => string;
}

export const STEP_TYPES: StepType[] = [
  {
    op: 'SET',
    name: 'Set',
    description: 'A setpoint, a loop mode, or an output value',
    fields: [
      { key: 'target', label: 'Target', kind: 'target',
        help: 'An output channel, or <loop>.setpoint / .manual / .mode' },
      { key: 'value', label: 'Value', kind: 'number' },
      { key: 'mode', label: 'Mode', kind: 'mode',
        help: 'Only for <loop>.mode' },
    ],
    defaults: () => ({ value: 0 }),
    summary: (step) =>
      step.target?.endsWith('.mode')
        ? `${step.target} → ${step.mode ?? 'off'}`
        : `${step.target ?? '—'} → ${step.value ?? 0}`,
  },
  {
    op: 'WAIT_UNTIL',
    name: 'Wait until',
    description: 'A condition on a channel — with a deadline, always',
    fields: [
      { key: 'channel', label: 'Channel', kind: 'channel' },
      { key: 'comparison', label: 'Comparison', kind: 'comparison' },
      { key: 'value', label: 'Value', kind: 'number' },
      { key: 'timeout_s', label: 'Timeout (s)', kind: 'seconds',
        help: 'Required. A wait that can last forever will' },
      { key: 'on_timeout', label: 'On timeout', kind: 'onTimeout' },
    ],
    defaults: () => ({ comparison: '>=', value: 0, timeout_s: 300, on_timeout: 'abort' }),
    summary: (step) =>
      `${step.channel ?? '—'} ${step.comparison ?? '>='} ${step.value ?? 0}` +
      ` · ${step.timeout_s ?? 0}s → ${step.on_timeout ?? 'abort'}`,
  },
  {
    op: 'RUN_FOR',
    name: 'Run for',
    description: 'Hold everything where it is — the body of the experiment',
    fields: [{ key: 'duration_s', label: 'Duration (s)', kind: 'seconds' }],
    defaults: () => ({ duration_s: 600 }),
    summary: (step) => `${step.duration_s ?? 0} s`,
  },
  {
    op: 'WAIT',
    name: 'Wait',
    description: 'A fixed delay',
    fields: [{ key: 'duration_s', label: 'Duration (s)', kind: 'seconds' }],
    defaults: () => ({ duration_s: 60 }),
    summary: (step) => `${step.duration_s ?? 0} s`,
  },
  {
    op: 'MARK_EVENT',
    name: 'Mark event',
    description: 'A labelled instant, kept in the run record',
    fields: [{ key: 'label', label: 'Label', kind: 'text' }],
    defaults: () => ({ label: '' }),
    summary: (step) => step.label || '—',
  },
  {
    op: 'ENABLE',
    name: 'Enable',
    description: 'A loop into automatic, or a device back on',
    fields: [{ key: 'target', label: 'Loop or device', kind: 'loopTarget' }],
    defaults: () => ({}),
    summary: (step) => step.target ?? '—',
  },
  {
    op: 'DISABLE',
    name: 'Disable',
    description: 'A loop off, or a device off',
    fields: [{ key: 'target', label: 'Loop or device', kind: 'loopTarget' }],
    defaults: () => ({}),
    summary: (step) => step.target ?? '—',
  },
  {
    op: 'STOP',
    name: 'Stop',
    description: 'Finish here, deliberately',
    fields: [],
    defaults: () => ({}),
    summary: () => 'end of scenario',
  },
];

export function stepType(op: string): StepType | undefined {
  return STEP_TYPES.find((type) => type.op === op);
}

/** What the step list shows for a step whose op this build does not know —
    the same honesty the dashboard owes an unknown widget type. */
export function describeStep(step: ExperimentStep): string {
  const type = stepType(step.op);
  return type ? type.summary(step) : `unknown step type "${step.op}"`;
}
