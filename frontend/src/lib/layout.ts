// =============================================================================
//  layout.ts — the 12-column grid, in about a hundred lines (§23).
//
//  WHY THIS IS NOT A LIBRARY
//  The bundle budget is 250 KiB gzip for the whole application, and it lives on
//  a 640 KB partition next to the user's configuration.  gridstack.js alone is
//  larger than everything shipped so far.  What a dashboard actually needs is:
//  snap to a column, push what you overlap downwards, and keep the order stable
//  on a phone.  That is this file.
//
//  RULES
//   * Positions are integer cells.  Nothing is stored in pixels, because a
//     layout built on a desktop has to be readable on a tablet.
//   * Collisions push DOWN, never sideways: horizontal position carries meaning
//     ("these two belong together") and vertical position mostly does not.
//   * The result is always compacted upwards, so deleting a widget does not
//     leave a hole that the operator has to tidy by hand.
// =============================================================================

import type { Widget } from './widgets';

export interface Rect {
  x: number;
  y: number;
  w: number;
  h: number;
}

export function overlaps(a: Rect, b: Rect): boolean {
  return a.x < b.x + b.w && a.x + a.w > b.x && a.y < b.y + b.h && a.y + a.h > b.y;
}

export function clampToGrid(rect: Rect, columns: number, min: { w: number; h: number }): Rect {
  const w = Math.max(min.w, Math.min(columns, Math.round(rect.w)));
  const h = Math.max(min.h, Math.round(rect.h));
  const x = Math.max(0, Math.min(columns - w, Math.round(rect.x)));
  const y = Math.max(0, Math.round(rect.y));
  return { x, y, w, h };
}

/**
 * Places `moved` at its new rectangle and pushes everything it overlaps down
 * until nothing overlaps.  Returns a NEW array; the caller decides whether to
 * commit it, which is what makes a dragged-but-cancelled move free.
 */
export function resolve(widgets: Widget[], movedId: string, rect: Rect): Widget[] {
  const next = widgets.map((widget) =>
    widget.id === movedId ? { ...widget, ...rect } : { ...widget },
  );

  const moved = next.find((widget) => widget.id === movedId);
  if (!moved) return next;

  // Settle in draw order, top to bottom.  A single pass is not enough: pushing
  // A down can make it collide with B, which then has to move too.
  const order = [moved, ...next.filter((w) => w.id !== movedId)
    .sort((a, b) => a.y - b.y || a.x - b.x)];

  const placed: Widget[] = [];
  for (const widget of order) {
    if (widget.id === movedId) {
      placed.push(widget);
      continue;
    }
    let candidate = { ...widget };
    // Bounded: every step moves the widget strictly downwards, and the grid is
    // never taller than the widget count times the tallest widget.
    let guard = 0;
    while (placed.some((other) => overlaps(candidate, other)) && guard < 256) {
      const blocking = placed.filter((other) => overlaps(candidate, other));
      const lowest = Math.max(...blocking.map((other) => other.y + other.h));
      candidate = { ...candidate, y: lowest };
      ++guard;
    }
    placed.push(candidate);
  }

  return compact(placed);
}

/**
 * Pulls every widget as far up as it will go without overlapping.  Deleting a
 * widget otherwise leaves a hole, and the operator has to drag six tiles to
 * close it — which is exactly the kind of chore that makes people give up on
 * editing the layout at all.
 */
export function compact(widgets: Widget[]): Widget[] {
  const sorted = [...widgets].sort((a, b) => a.y - b.y || a.x - b.x);
  const placed: Widget[] = [];
  for (const widget of sorted) {
    let candidate = { ...widget };
    while (candidate.y > 0) {
      const lifted = { ...candidate, y: candidate.y - 1 };
      if (placed.some((other) => overlaps(lifted, other))) break;
      candidate = lifted;
    }
    placed.push(candidate);
  }
  return placed;
}

/** The first free rectangle of this size, scanning left to right, top down. */
export function findSlot(widgets: Widget[], columns: number,
                         size: { w: number; h: number }): Rect {
  const w = Math.min(columns, size.w);
  const maxY = widgets.reduce((tallest, widget) =>
    Math.max(tallest, widget.y + widget.h), 0);
  for (let y = 0; y <= maxY; ++y) {
    for (let x = 0; x + w <= columns; ++x) {
      const candidate = { x, y, w, h: size.h };
      if (!widgets.some((other) => overlaps(candidate, other))) return candidate;
    }
  }
  return { x: 0, y: maxY, w, h: size.h };
}

/** Rows the grid needs, so the container can be sized without measuring. */
export function gridRows(widgets: Widget[]): number {
  return widgets.reduce((rows, widget) => Math.max(rows, widget.y + widget.h), 0);
}

/**
 * Phone layout: one column, in the reading order of the desktop layout.
 * Order is preserved rather than recomputed — the operator arranged those tiles
 * for a reason, and a phone should show the same story in a narrower shape.
 */
export function singleColumnOrder(widgets: Widget[]): Widget[] {
  return [...widgets].sort((a, b) => a.y - b.y || a.x - b.x);
}
