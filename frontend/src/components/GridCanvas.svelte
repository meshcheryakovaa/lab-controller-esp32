<script lang="ts">
  // ===========================================================================
  //  GridCanvas — the dashboard surface (§23, §26).
  //
  //  VIEW mode is a plain CSS grid and costs nothing.  EDIT mode adds pointer
  //  handlers for dragging and resizing.  Pointer events, not mouse events:
  //  the operator standing at the rig is holding a tablet, and a dashboard that
  //  can only be rearranged with a mouse is a dashboard that is never
  //  rearranged.
  //
  //  Geometry lives in integer cells (see lib/layout.ts).  Nothing is stored in
  //  pixels, so a layout built on a desktop still means something on a phone.
  // ===========================================================================
  import { clampToGrid, gridRows, resolve, singleColumnOrder } from '../lib/layout';
  import type { Rect } from '../lib/layout';
  import { widgetType } from '../lib/widgets';
  import type { Widget } from '../lib/widgets';

  let {
    widgets = $bindable<Widget[]>([]),
    columns = 12,
    rowHeight = 40,
    editing = false,
    narrow = false,
    selectedId = $bindable<string>(''),
    onchange = () => {},
    onedit = (_id: string) => {},
    onremove = (_id: string) => {},
  }: {
    widgets?: Widget[];
    columns?: number;
    rowHeight?: number;
    editing?: boolean;
    narrow?: boolean;
    selectedId?: string;
    onchange?: () => void;
    onedit?: (id: string) => void;
    onremove?: (id: string) => void;
  } = $props();

  const GAP = 8;

  let surface: HTMLDivElement | undefined = $state();
  let drag = $state<{
    id: string;
    mode: 'move' | 'resize';
    startX: number;
    startY: number;
    origin: Rect;
  } | null>(null);
  // The rectangle under the pointer right now, shown as an outline so the
  // operator sees where the tile will land before letting go.
  let ghost = $state<Rect | null>(null);

  const rows = $derived(Math.max(gridRows(widgets), editing ? 6 : 1));
  const ordered = $derived(narrow ? singleColumnOrder(widgets) : widgets);

  function cellWidth(): number {
    const width = surface?.clientWidth ?? 0;
    return (width - GAP * (columns - 1)) / columns;
  }

  function begin(event: PointerEvent, widget: Widget, mode: 'move' | 'resize') {
    if (!editing || narrow) return;
    event.preventDefault();
    event.stopPropagation();
    (event.currentTarget as HTMLElement).setPointerCapture(event.pointerId);
    selectedId = widget.id;
    drag = {
      id: widget.id,
      mode,
      startX: event.clientX,
      startY: event.clientY,
      origin: { x: widget.x, y: widget.y, w: widget.w, h: widget.h },
    };
    ghost = { ...drag.origin };
  }

  function move(event: PointerEvent) {
    if (!drag) return;
    const cw = cellWidth() + GAP;
    const ch = rowHeight + GAP;
    const dx = Math.round((event.clientX - drag.startX) / cw);
    const dy = Math.round((event.clientY - drag.startY) / ch);
    const type = widgetType(widgets.find((w) => w.id === drag!.id)?.type ?? '');
    const min = type?.minSize ?? { w: 1, h: 1 };

    const raw: Rect = drag.mode === 'move'
      ? { ...drag.origin, x: drag.origin.x + dx, y: drag.origin.y + dy }
      : { ...drag.origin, w: drag.origin.w + dx, h: drag.origin.h + dy };
    ghost = clampToGrid(raw, columns, min);
  }

  function end() {
    if (!drag || !ghost) {
      drag = null;
      ghost = null;
      return;
    }
    const changed = ghost.x !== drag.origin.x || ghost.y !== drag.origin.y ||
                    ghost.w !== drag.origin.w || ghost.h !== drag.origin.h;
    if (changed) {
      widgets = resolve(widgets, drag.id, ghost);
      // One change, one notification.  The caller debounces the write; a drag
      // that emits per pointer move would mean a hundred writes to flash (R4).
      onchange();
    }
    drag = null;
    ghost = null;
  }

  function nudge(event: KeyboardEvent, widget: Widget) {
    if (!editing || narrow) return;
    const step: Record<string, [number, number]> = {
      ArrowLeft: [-1, 0], ArrowRight: [1, 0], ArrowUp: [0, -1], ArrowDown: [0, 1],
    };
    const delta = step[event.key];
    if (!delta) return;
    event.preventDefault();
    const type = widgetType(widget.type);
    const rect = clampToGrid(
      { x: widget.x + delta[0], y: widget.y + delta[1], w: widget.w, h: widget.h },
      columns, type?.minSize ?? { w: 1, h: 1 },
    );
    widgets = resolve(widgets, widget.id, rect);
    onchange();
  }

  function style(widget: Widget): string {
    if (narrow) return '';
    return `grid-column: ${widget.x + 1} / span ${widget.w};` +
           `grid-row: ${widget.y + 1} / span ${widget.h};`;
  }
</script>

<svelte:window onpointermove={move} onpointerup={end} onpointercancel={end} />

<div
  class="surface"
  class:editing
  class:narrow
  bind:this={surface}
  style="--columns: {columns}; --row-height: {rowHeight}px; --rows: {rows}; --gap: {GAP}px;">

  {#each ordered as widget (widget.id)}
    {@const type = widgetType(widget.type)}
    <!-- The tile really is interactive in EDIT mode — it is the drag target and
         it accepts arrow keys — and inert in VIEW mode, where it gets neither a
         role nor a tab stop.  The checker cannot see the two modes apart. -->
    <!-- svelte-ignore a11y_no_noninteractive_tabindex -->
    <div
      class="cell"
      class:selected={editing && selectedId === widget.id}
      class:dragging={drag?.id === widget.id}
      style={style(widget)}
      role={editing ? 'button' : undefined}
      tabindex={editing ? 0 : undefined}
      aria-label={editing ? `${widget.type} widget ${widget.id}` : undefined}
      onkeydown={(e) => nudge(e, widget)}
      onpointerdown={(e) => begin(e, widget, 'move')}>

      {#if type}
        <type.component config={widget.config} height={widget.h * rowHeight + (widget.h - 1) * GAP - 16} />
      {:else}
        <!-- A dashboard imported from a newer build can name a widget this
             build has never heard of.  Saying so beats an empty rectangle. -->
        <div class="unknown">
          <strong>Unknown widget</strong>
          <code>{widget.type}</code>
          <span>This firmware's interface has no such widget type.</span>
        </div>
      {/if}

      {#if editing && !narrow}
        <div class="tools">
          <button type="button" title="Settings"
                  onpointerdown={(e) => e.stopPropagation()}
                  onclick={() => onedit(widget.id)}>⚙</button>
          <button type="button" class="danger" title="Remove"
                  onpointerdown={(e) => e.stopPropagation()}
                  onclick={() => onremove(widget.id)}>×</button>
        </div>
        <div class="handle" role="presentation"
             onpointerdown={(e) => begin(e, widget, 'resize')}></div>
      {/if}
    </div>
  {/each}

  {#if ghost && drag}
    <div class="ghost" style="grid-column: {ghost.x + 1} / span {ghost.w};
                              grid-row: {ghost.y + 1} / span {ghost.h};"></div>
  {/if}
</div>

<style>
  .surface {
    display: grid;
    grid-template-columns: repeat(var(--columns), minmax(0, 1fr));
    grid-auto-rows: var(--row-height);
    grid-template-rows: repeat(var(--rows), var(--row-height));
    gap: var(--gap);
    min-height: calc(var(--rows) * var(--row-height));
  }
  .surface.narrow { display: grid; grid-template-columns: 1fr;
                    grid-template-rows: none; grid-auto-rows: minmax(120px, auto); }
  .surface.editing {
    /* The grid itself becomes visible only while editing: in VIEW mode it would
       be decoration competing with the measurements. */
    background-image:
      repeating-linear-gradient(to right, var(--line) 0 1px, transparent 1px
        calc((100% + var(--gap)) / var(--columns)));
    background-position: 0 0;
    border-radius: 8px;
  }
  .cell {
    background: var(--surface); border: 1px solid var(--line); border-radius: 8px;
    padding: 0.5rem 0.6rem; min-width: 0; min-height: 0; overflow: hidden;
    position: relative; container-type: inline-size;
  }
  .surface.editing .cell { cursor: grab; }
  .cell.dragging { opacity: 0.5; }
  .cell.selected { border-color: var(--accent); }
  .cell:focus-visible { outline: 2px solid var(--accent); outline-offset: 1px; }
  .tools { position: absolute; top: 2px; right: 2px; display: flex; gap: 2px;
           opacity: 0; transition: opacity 0.1s; }
  .cell:hover .tools, .cell.selected .tools { opacity: 1; }
  .tools button { background: var(--surface-2); border: 1px solid var(--line);
                  color: var(--muted); border-radius: 4px; width: 1.3rem;
                  height: 1.3rem; line-height: 1; cursor: pointer; font-size: 0.75rem;
                  display: inline-flex; align-items: center; justify-content: center; }
  .tools button.danger:hover { color: var(--danger); border-color: var(--danger); }
  .handle { position: absolute; right: 0; bottom: 0; width: 14px; height: 14px;
            cursor: nwse-resize;
            background: linear-gradient(135deg, transparent 50%, var(--line) 50%); }
  .ghost { border: 1px dashed var(--accent); border-radius: 8px;
           background: color-mix(in srgb, var(--accent) 10%, transparent);
           pointer-events: none; }
  .unknown { height: 100%; display: grid; align-content: center; gap: 0.15rem;
             text-align: center; font-size: 0.75rem; color: var(--warn); }
  .unknown code { font-family: ui-monospace, SFMono-Regular, monospace;
                  font-size: 0.72rem; }
  .unknown span { color: var(--muted); font-size: 0.68rem; }
</style>
