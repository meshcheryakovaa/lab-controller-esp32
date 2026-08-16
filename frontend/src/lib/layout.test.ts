// =============================================================================
//  layout.test.ts — the grid engine (npm test).
//
//  This is the one piece of the web interface where a bug produces a WRONG
//  RESULT rather than an ugly one: two widgets on the same cells, a tile pushed
//  off the grid, a hole that never closes.  §58 says the things to test first
//  are the ones where an error gives a quiet wrong answer instead of a crash —
//  the same reason the formula parser and the calibration solver have tests.
// =============================================================================

import { describe, expect, it } from 'vitest';
import { clampToGrid, compact, findSlot, gridRows, overlaps, resolve,
         singleColumnOrder } from './layout';
import type { Widget } from './widgets';

function w(id: string, x: number, y: number, width: number, height: number): Widget {
  return { id, type: 'value', x, y, w: width, h: height, config: {} };
}

function anyOverlap(widgets: Widget[]): boolean {
  for (let i = 0; i < widgets.length; ++i) {
    for (let j = i + 1; j < widgets.length; ++j) {
      if (overlaps(widgets[i]!, widgets[j]!)) return true;
    }
  }
  return false;
}

describe('clampToGrid', () => {
  it('keeps a widget inside the columns it has', () => {
    expect(clampToGrid({ x: 10, y: 0, w: 4, h: 2 }, 12, { w: 1, h: 1 }))
      .toEqual({ x: 8, y: 0, w: 4, h: 2 });
  });

  it('refuses to make a widget smaller than its own minimum', () => {
    expect(clampToGrid({ x: 0, y: 0, w: 1, h: 1 }, 12, { w: 4, h: 3 }))
      .toEqual({ x: 0, y: 0, w: 4, h: 3 });
  });

  it('never produces a negative position', () => {
    expect(clampToGrid({ x: -5, y: -5, w: 2, h: 2 }, 12, { w: 1, h: 1 }))
      .toEqual({ x: 0, y: 0, w: 2, h: 2 });
  });

  it('cannot make a widget wider than the grid', () => {
    const rect = clampToGrid({ x: 0, y: 0, w: 40, h: 1 }, 12, { w: 1, h: 1 });
    expect(rect.w).toBe(12);
    expect(rect.x).toBe(0);
  });
});

describe('resolve', () => {
  it('pushes what it overlaps downwards and leaves nothing overlapping', () => {
    const before = [w('a', 0, 0, 6, 2), w('b', 0, 2, 6, 2), w('c', 0, 4, 6, 2)];
    const after = resolve(before, 'c', { x: 0, y: 0, w: 6, h: 2 });

    expect(after.find((x) => x.id === 'c')).toMatchObject({ x: 0, y: 0 });
    expect(anyOverlap(after)).toBe(false);
    // Everything it displaced is still there — pushing is not deleting.
    expect(after.map((x) => x.id).sort()).toEqual(['a', 'b', 'c']);
  });

  it('settles a chain: pushing A into B pushes B into C', () => {
    const before = [w('a', 0, 0, 12, 1), w('b', 0, 1, 12, 1), w('c', 0, 2, 12, 1)];
    const after = resolve(before, 'c', { x: 0, y: 0, w: 12, h: 1 });
    expect(anyOverlap(after)).toBe(false);
    const rows = after.map((x) => x.y).sort();
    expect(rows).toEqual([0, 1, 2]);
  });

  it('does not disturb widgets it never touches', () => {
    const before = [w('a', 0, 0, 3, 2), w('b', 9, 0, 3, 2)];
    const after = resolve(before, 'a', { x: 3, y: 0, w: 3, h: 2 });
    expect(after.find((x) => x.id === 'b')).toMatchObject({ x: 9, y: 0 });
  });

  it('closes the gap it leaves behind', () => {
    // Moving the top widget away must not leave everything else floating one
    // row lower than it needs to be.
    const before = [w('a', 0, 0, 12, 2), w('b', 0, 2, 6, 2)];
    const after = resolve(before, 'a', { x: 0, y: 6, w: 12, h: 2 });
    expect(after.find((x) => x.id === 'b')).toMatchObject({ y: 0 });
  });
});

describe('compact', () => {
  it('pulls widgets up into holes', () => {
    const after = compact([w('a', 0, 4, 3, 2), w('b', 6, 9, 3, 2)]);
    expect(after.every((x) => x.y === 0)).toBe(true);
  });

  it('preserves stacking order in a column', () => {
    const after = compact([w('top', 0, 3, 3, 1), w('bottom', 0, 8, 3, 1)]);
    expect(after.find((x) => x.id === 'top')!.y)
      .toBeLessThan(after.find((x) => x.id === 'bottom')!.y);
  });

  it('is idempotent', () => {
    const once = compact([w('a', 0, 5, 4, 2), w('b', 4, 7, 4, 2)]);
    expect(compact(once)).toEqual(once);
  });
});

describe('findSlot', () => {
  it('uses the first free space on the top row', () => {
    expect(findSlot([w('a', 0, 0, 3, 2)], 12, { w: 3, h: 2 }))
      .toEqual({ x: 3, y: 0, w: 3, h: 2 });
  });

  it('starts a new row when the current one is full', () => {
    const full = [w('a', 0, 0, 6, 2), w('b', 6, 0, 6, 2)];
    expect(findSlot(full, 12, { w: 6, h: 2 }).y).toBe(2);
  });

  it('never proposes a slot that overlaps something', () => {
    const widgets = [w('a', 0, 0, 4, 2), w('b', 5, 0, 3, 3), w('c', 2, 3, 6, 2)];
    const slot = findSlot(widgets, 12, { w: 4, h: 2 });
    expect(widgets.some((x) => overlaps({ ...slot }, x))).toBe(false);
  });

  it('clips a widget wider than the grid instead of placing it off the edge', () => {
    const slot = findSlot([], 12, { w: 20, h: 2 });
    expect(slot.w).toBe(12);
    expect(slot.x).toBe(0);
  });
});

describe('the phone layout', () => {
  it('reads left to right, top to bottom — the order the operator arranged', () => {
    const desktop = [w('right', 6, 0, 6, 2), w('left', 0, 0, 6, 2), w('below', 0, 2, 12, 2)];
    expect(singleColumnOrder(desktop).map((x) => x.id))
      .toEqual(['left', 'right', 'below']);
  });
});

describe('gridRows', () => {
  it('counts the rows the tallest stack needs', () => {
    expect(gridRows([w('a', 0, 0, 3, 2), w('b', 3, 4, 3, 3)])).toBe(7);
  });

  it('is zero for an empty dashboard', () => {
    expect(gridRows([])).toBe(0);
  });
});
