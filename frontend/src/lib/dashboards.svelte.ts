// =============================================================================
//  dashboards.svelte.ts — the dashboard the operator is looking at (§22, R4).
//
//  THE SAVE POLICY IS THE INTERESTING PART.
//  Dragging a widget across a 12-column grid emits a change per snap.  Writing
//  each one would be a hundred writes to a flash part that is rated for about
//  ten thousand erase cycles per sector, to record a layout nobody has finished
//  arranging yet.  So edits are held for two seconds after the LAST one and
//  written once — and, because two seconds is long enough for the operator to
//  close the tab, an unsaved layout is flushed on the way out too.
//
//  The state is deliberately not merged into ControllerState: descriptors and
//  values describe the rig, and a dashboard describes how somebody wants to
//  look at it.  They change for entirely different reasons.
// =============================================================================

import { api } from './api';
import { describe } from './state.svelte';
import type { Dashboard, DashboardSummary, Widget } from './widgets';

const SAVE_DEBOUNCE_MS = 2000;

export function makeDefaultDashboard(key: string, name: string): Dashboard {
  return { key, name, grid: { columns: 12, row_height: 40 }, widgets: [] };
}

export class DashboardState {
  list = $state<DashboardSummary[]>([]);
  current = $state<Dashboard | null>(null);
  currentKey = $state('');

  loading = $state(false);
  saving = $state(false);
  dirty = $state(false);
  error = $state('');
  /** Widgets whose channel the firmware could not find, by widget id. */
  danglingChannels = $state(0);

  limits = $state({ dashboards: 8, widgets: 24, columns: 12 });

  private timer = 0;

  async refreshList(): Promise<void> {
    this.error = '';
    try {
      const response = await api.dashboards();
      this.list = response.dashboards;
      if (response.limits) this.limits = response.limits;
    } catch (e) {
      this.error = describe(e);
    }
  }

  async open(key: string): Promise<void> {
    if (!key) {
      this.current = null;
      this.currentKey = '';
      return;
    }
    // A pending edit belongs to the dashboard being left, not the one being
    // opened.  Flushing first is the difference between "saved" and "lost".
    await this.flush();
    this.loading = true;
    this.error = '';
    try {
      const dashboard = await api.dashboard(key);
      this.current = normalise(dashboard);
      this.currentKey = key;
      this.danglingChannels = Number(dashboard.health?.dangling_channels ?? 0);
      this.dirty = false;
    } catch (e) {
      this.error = describe(e);
      this.current = null;
      this.currentKey = '';
    } finally {
      this.loading = false;
    }
  }

  /** Called by the editor after any change to the current dashboard. */
  touch(): void {
    if (!this.current) return;
    this.dirty = true;
    clearTimeout(this.timer);
    this.timer = window.setTimeout(() => void this.flush(), SAVE_DEBOUNCE_MS);
  }

  async flush(): Promise<void> {
    clearTimeout(this.timer);
    this.timer = 0;
    if (!this.dirty || !this.current) return;

    const snapshot = $state.snapshot(this.current) as Dashboard;
    this.saving = true;
    try {
      const saved = await api.saveDashboard(snapshot.key, snapshot);
      this.danglingChannels = Number(saved.health?.dangling_channels ?? 0);
      this.dirty = false;
      this.error = '';
      await this.refreshList();
    } catch (e) {
      // Deliberately keeps `dirty` set: the layout on screen is not on the
      // board, and pretending otherwise is how work disappears.
      this.error = describe(e);
    } finally {
      this.saving = false;
    }
  }

  async create(key: string, name: string): Promise<boolean> {
    this.error = '';
    try {
      await api.createDashboard(makeDefaultDashboard(key, name));
      await this.refreshList();
      await this.open(key);
      return true;
    } catch (e) {
      this.error = describe(e);
      return false;
    }
  }

  async remove(key: string): Promise<void> {
    this.error = '';
    try {
      clearTimeout(this.timer);
      this.dirty = false;
      await api.deleteDashboard(key);
      if (this.currentKey === key) {
        this.current = null;
        this.currentKey = '';
      }
      await this.refreshList();
      if (!this.currentKey && this.list.length > 0) await this.open(this.list[0]!.key);
    } catch (e) {
      this.error = describe(e);
    }
  }

  /** Unique within the current dashboard; ids are stable once assigned. */
  nextWidgetId(): string {
    const used = new Set((this.current?.widgets ?? []).map((w) => w.id));
    for (let i = 1; i <= 999; ++i) {
      const candidate = `w${i}`;
      if (!used.has(candidate)) return candidate;
    }
    return `w${Date.now()}`;
  }

  atCapacity(): boolean {
    return (this.current?.widgets.length ?? 0) >= this.limits.widgets;
  }
}

/** Fills in what an older or hand-written document may have left out. */
function normalise(raw: any): Dashboard {
  const widgets: Widget[] = (raw.widgets ?? []).map((widget: any) => ({
    id: String(widget.id),
    type: String(widget.type),
    x: Number(widget.x ?? 0),
    y: Number(widget.y ?? 0),
    w: Math.max(1, Number(widget.w ?? 1)),
    h: Math.max(1, Number(widget.h ?? 1)),
    config: widget.config ?? {},
  }));
  return {
    key: String(raw.key),
    name: String(raw.name ?? raw.key),
    grid: {
      columns: Number(raw.grid?.columns ?? 12),
      row_height: Number(raw.grid?.row_height ?? 40),
    },
    widgets,
  };
}

export const dashboards = new DashboardState();
