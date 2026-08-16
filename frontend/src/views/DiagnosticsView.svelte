<script lang="ts">
  import { onMount } from 'svelte';
  import { formatBytes, formatInterval } from '../lib/format';
  import { controller } from '../lib/state.svelte';

  onMount(() => {
    void controller.refreshDiagnostics();
    const timer = setInterval(() => void controller.refreshDiagnostics(), 3000);
    return () => clearInterval(timer);
  });

  const d = $derived(controller.diagnostics ?? {});
  const tasks = $derived((d.tasks ?? []) as any[]);
  const heap = $derived((d.heap ?? {}) as any);

  const priorityName = ['safety', 'control', 'acquisition', 'processing',
                        'telemetry', 'background'];
</script>

<div class="page">
  <section class="panel wide">
    <h2>Scheduler</h2>
    <p class="note">
      Duration, lateness and overruns per task. This table is how a stalled
      control loop is traced back to the task responsible — by name, not by
      guesswork.
    </p>
    <table>
      <thead>
        <tr>
          <th>Task</th><th>Priority</th><th class="right">Rate</th>
          <th class="right">Runs</th><th class="right">Last</th>
          <th class="right">Max</th><th class="right">Lateness</th>
          <th class="right">Overruns</th><th class="right">Misses</th>
        </tr>
      </thead>
      <tbody>
        {#each tasks as task (task.name)}
          <tr class:trouble={task.overruns > 0}>
            <td>{task.name}</td>
            <td class="muted">{priorityName[task.priority] ?? task.priority}</td>
            <td class="right numeric">{formatInterval(task.period_us)}</td>
            <td class="right numeric muted">{task.runs}</td>
            <td class="right numeric">{task.last_us} µs</td>
            <td class="right numeric">{task.max_us} µs</td>
            <td class="right numeric">{task.max_lateness_us} µs</td>
            <td class="right numeric" class:bad={task.overruns > 0}>{task.overruns}</td>
            <td class="right numeric" class:bad={task.misses > 0}>{task.misses}</td>
          </tr>
        {/each}
      </tbody>
    </table>
  </section>

  <section class="panel">
    <h2>Memory</h2>
    <dl>
      <dt>Free heap</dt><dd class="numeric">{formatBytes(heap.free)}</dd>
      <!-- The low-water mark is the number that predicts a crash; the current
           free heap only says how lucky you are right now. -->
      <dt>Minimum ever</dt><dd class="numeric">{formatBytes(heap.min_free)}</dd>
      <dt>Largest block</dt><dd class="numeric">{formatBytes(heap.largest_block)}</dd>
      <dt>Total</dt><dd class="numeric">{formatBytes(heap.total)}</dd>
    </dl>
    {#if heap.largest_block !== undefined && heap.free > 0}
      <p class="note">
        Fragmentation: largest free block is
        {Math.round((heap.largest_block / heap.free) * 100)}% of free heap.
      </p>
    {/if}
  </section>

  <section class="panel">
    <h2>Data plane</h2>
    <dl>
      <dt>Loop passes</dt><dd class="numeric">{d.loop?.passes ?? '—'}</dd>
      <dt>Longest pass</dt><dd class="numeric">{d.loop?.max_pass_us ?? '—'} µs</dd>
      <dt>Budget exhausted</dt>
      <dd class="numeric" class:bad={(d.loop?.budget_exhausted ?? 0) > 0}>
        {d.loop?.budget_exhausted ?? '—'}
      </dd>
      <dt>Samples published</dt><dd class="numeric">{d.data?.published_samples ?? '—'}</dd>
      <dt>Samples suppressed</dt><dd class="numeric">{d.data?.suppressed_samples ?? '—'}</dd>
      <dt>Active channels</dt><dd class="numeric">{d.data?.active_channels ?? '—'}</dd>
      <dt>Events dropped</dt>
      <dd class="numeric" class:bad={(d.events?.dropped ?? 0) > 0}>
        {d.events?.dropped ?? '—'}
      </dd>
      <dt>API errors</dt><dd class="numeric">{d.api?.errors ?? '—'}</dd>
    </dl>
  </section>

  <section class="panel">
    <h2>Buses</h2>
    {#if (d.i2c ?? []).length === 0}
      <p class="muted">No I²C bus configured.</p>
    {:else}
      <dl>
        {#each d.i2c as bus (bus.index)}
          <dt>I²C {bus.index}</dt>
          <dd class="numeric" class:bad={bus.errors > 0}>{bus.errors} errors</dd>
        {/each}
      </dl>
      <p class="note">
        An occasional NACK is normal. A steadily climbing count is a bad solder
        joint or a missing pull-up.
      </p>
    {/if}
  </section>
</div>

<style>
  .page { display: grid; gap: 1rem;
          grid-template-columns: repeat(auto-fit, minmax(300px, 1fr)); }
  .panel { background: var(--surface); border: 1px solid var(--line);
           border-radius: 8px; padding: 0.8rem 0.9rem; display: grid;
           gap: 0.5rem; align-content: start; }
  .wide { grid-column: 1 / -1; }
  h2 { margin: 0; font-size: 0.8rem; text-transform: uppercase;
       letter-spacing: 0.07em; color: var(--muted); font-weight: 600; }
  table { width: 100%; border-collapse: collapse; font-size: 0.8rem; }
  th { text-align: left; font-weight: 500; font-size: 0.66rem; text-transform: uppercase;
       letter-spacing: 0.05em; color: var(--muted); padding-bottom: 0.3rem; }
  th.right, td.right { text-align: right; }
  td { padding: 0.25rem 0.4rem 0.25rem 0; border-top: 1px solid var(--line); }
  tr.trouble { background: rgba(210, 153, 34, 0.08); }
  dl { display: grid; grid-template-columns: 10rem 1fr; gap: 0.25rem 1rem;
       margin: 0; font-size: 0.85rem; }
  dt { color: var(--muted); font-size: 0.75rem; }
  dd { margin: 0; }
  .muted { color: var(--muted); }
  .bad { color: var(--warn); }
  .note { font-size: 0.75rem; color: var(--muted); margin: 0; }
</style>
