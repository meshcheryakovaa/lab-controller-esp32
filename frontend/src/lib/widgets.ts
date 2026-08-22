// =============================================================================
//  widgets.ts — THE registry of widget types (§24, ADR-0015).
//
//  This file is to the dashboard what BuiltinModules.cpp is to the firmware:
//  the single list, and the only place that has to change when a type is added.
//  A new widget is one component plus one entry here — no firmware change, no
//  edits to the grid, the editor or the save path.
//
//  The firmware stores `type` as an opaque string and never validates it, so
//  this list is the whole vocabulary.  Which means a dashboard imported from a
//  newer build can name a type that is not here, and the canvas has to say so
//  rather than render an empty box.
// =============================================================================

import type { Component } from 'svelte';
import ChartWidget from '../components/widgets/ChartWidget.svelte';
import OutputWidget from '../components/widgets/OutputWidget.svelte';
import GaugeWidget from '../components/widgets/GaugeWidget.svelte';
import LoopWidget from '../components/widgets/LoopWidget.svelte';
import RunWidget from '../components/widgets/RunWidget.svelte';
import StateWidget from '../components/widgets/StateWidget.svelte';
import TextWidget from '../components/widgets/TextWidget.svelte';
import ValueWidget from '../components/widgets/ValueWidget.svelte';

export interface WidgetSeries {
  channel: string;
  axis?: 'left' | 'right';
  color?: string;
}

export interface WidgetConfig {
  /** Single-channel widgets. */
  channel?: string;
  /** Loop widgets, by control-loop id. */
  loop?: string;
  /** Multi-channel widgets (chart). */
  series?: WidgetSeries[];
  title?: string;
  text?: string;
  precision?: number;
  min?: number;
  max?: number;
  window_s?: number;
  [key: string]: unknown;
}

export interface Widget {
  id: string;
  type: string;
  x: number;
  y: number;
  w: number;
  h: number;
  config: WidgetConfig;
}

export interface DashboardGrid {
  columns: number;
  row_height: number;
}

export interface Dashboard {
  key: string;
  name: string;
  grid: DashboardGrid;
  widgets: Widget[];
}

export interface DashboardSummary {
  key: string;
  name: string;
  widgets: number;
  dangling_channels?: number;
}

/** How the editor asks for a widget's settings. Deliberately tiny. */
export type FieldKind = 'channel' | 'channels' | 'text' | 'number' | 'loop' | 'choice';

export interface FieldOption {
  value: number | string;
  label: string;
}

export interface WidgetField {
  key: string;
  label: string;
  kind: FieldKind;
  help?: string;
  min?: number;
  max?: number;
  /** For `choice`: the whole vocabulary of the field. */
  options?: FieldOption[];
}

export interface WidgetType {
  id: string;
  name: string;
  description: string;
  component: Component<any>;
  /** Default footprint in grid cells. */
  defaultSize: { w: number; h: number };
  minSize: { w: number; h: number };
  fields: WidgetField[];
  /** Seeds config when the widget is created. */
  defaults: (channelKey?: string) => WidgetConfig;
}

export const WIDGET_TYPES: WidgetType[] = [
  {
    id: 'value',
    name: 'Value',
    description: 'One reading, large, with its unit and quality',
    component: ValueWidget,
    defaultSize: { w: 3, h: 2 },
    minSize: { w: 2, h: 2 },
    fields: [
      { key: 'channel', label: 'Channel', kind: 'channel' },
      { key: 'precision', label: 'Decimals', kind: 'number', min: 0, max: 6,
        help: 'Leave empty to use the channel’s own precision' },
    ],
    defaults: (channel) => ({ channel }),
  },
  {
    id: 'chart',
    name: 'Chart',
    description: 'Several channels against time, up to three units',
    component: ChartWidget,
    defaultSize: { w: 8, h: 5 },
    minSize: { w: 4, h: 3 },
    fields: [
      { key: 'series', label: 'Channels', kind: 'channels' },
      // A free number here was a promise the browser could not keep: it kept
      // three minutes of history, so "3600" drew three minutes and said an
      // hour.  The three the chart can actually show are the three offered.
      { key: 'window_s', label: 'Default range', kind: 'choice',
        options: [
          { value: 0, label: 'All time' },
          { value: 300, label: 'Last 5 minutes' },
          { value: 600, label: 'Last 10 minutes' },
        ],
        help: 'Where the buttons on the tile start; the operator can change it '
            + 'without saving the dashboard' },
    ],
    defaults: (channel) => ({
      series: channel ? [{ channel }] : [],
      window_s: 300,
    }),
  },
  {
    id: 'gauge',
    name: 'Gauge',
    description: 'A reading against a declared range — position at a glance',
    component: GaugeWidget,
    defaultSize: { w: 3, h: 3 },
    minSize: { w: 2, h: 2 },
    fields: [
      { key: 'channel', label: 'Channel', kind: 'channel' },
      { key: 'min', label: 'Range from', kind: 'number' },
      { key: 'max', label: 'Range to', kind: 'number' },
    ],
    // A gauge without a range is a decoration: seed it from the channel's own
    // declared limits and let the operator override.
    defaults: (channel) => ({ channel }),
  },
  {
    id: 'run',
    name: 'Experiment',
    description: 'The running scenario: which step, how long left, and a way to stop it',
    component: RunWidget,
    defaultSize: { w: 3, h: 3 },
    minSize: { w: 2, h: 2 },
    fields: [{ key: 'title', label: 'Title', kind: 'text' }],
    defaults: () => ({}),
  },
  {
    id: 'loop',
    name: 'Loop',
    description: 'A PID loop: measurement, setpoint and mode in one place',
    component: LoopWidget,
    defaultSize: { w: 3, h: 4 },
    minSize: { w: 2, h: 3 },
    fields: [
      { key: 'loop', label: 'Control loop', kind: 'loop',
        help: 'Loops are defined on the Control page' },
      { key: 'title', label: 'Title', kind: 'text' },
    ],
    defaults: () => ({}),
  },
  {
    id: 'output',
    // Named "Output", not "Control": with a Control page full of loops next to
    // it, a widget called Control that commands one actuator directly is a
    // promise about where authority lives that the page does not keep.  The
    // stored `type` is unchanged, so existing dashboards are unaffected.
    name: 'Output',
    description: 'Command an output — with its safe state and its deadline in view',
    component: OutputWidget,
    defaultSize: { w: 3, h: 3 },
    minSize: { w: 2, h: 3 },
    fields: [
      { key: 'channel', label: 'Output channel', kind: 'channel',
        help: 'Only output channels can be commanded' },
    ],
    defaults: () => ({}),
  },
  {
    id: 'state',
    name: 'Device state',
    description: 'Whether a device is running, and what went wrong if not',
    component: StateWidget,
    defaultSize: { w: 3, h: 2 },
    minSize: { w: 2, h: 1 },
    fields: [
      { key: 'device', label: 'Device', kind: 'text',
        help: 'Device key, or leave empty to show every device that is not healthy' },
    ],
    defaults: () => ({}),
  },
  {
    id: 'text',
    name: 'Note',
    description: 'A label on the layout — what the rig is, what to watch',
    component: TextWidget,
    defaultSize: { w: 4, h: 2 },
    minSize: { w: 2, h: 1 },
    fields: [
      { key: 'title', label: 'Title', kind: 'text' },
      { key: 'text', label: 'Text', kind: 'text' },
    ],
    defaults: () => ({ title: 'Note', text: '' }),
  },
];

export function widgetType(id: string): WidgetType | undefined {
  return WIDGET_TYPES.find((type) => type.id === id);
}

/** Every channel a widget depends on, whatever shape it stores them in. */
export function widgetChannels(widget: Widget): string[] {
  const keys: string[] = [];
  if (widget.config.channel) keys.push(widget.config.channel);
  for (const entry of widget.config.series ?? []) {
    if (entry.channel) keys.push(entry.channel);
  }
  return keys;
}
