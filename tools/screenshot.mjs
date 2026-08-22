// =============================================================================
//  tools/screenshot.mjs — drives the real UI against the real firmware API.
//
//  Not a mock and not a storybook: `tools/host_server` links the actual
//  RestApi, DeviceManager and drivers, so what these screenshots show is what
//  the ESP32 serves.  The Add Device sequence below is the Milestone 4
//  acceptance criterion executed end to end — pick a module, get a real
//  validation error from the real ResourceManager, fix it, create the device.
//
//  Usage:  node tools/screenshot.mjs [baseUrl] [outputDir]
// =============================================================================
import { chromium, request } from 'playwright';
import { mkdirSync, readFileSync } from 'node:fs';

const base = process.argv[2] ?? 'http://127.0.0.1:8080';
const out = process.argv[3] ?? '/home/claude/shots';
mkdirSync(out, { recursive: true });

// The sandbox ships a Chromium that may not match the Playwright build id;
// point at it explicitly rather than downloading a second copy.
const executablePath = process.env.CHROMIUM_PATH ?? undefined;
const browser = await chromium.launch(executablePath ? { executablePath } : {});
const page = await browser.newPage({
  viewport: { width: 1280, height: 860 },
  deviceScaleFactor: 2,
});

const shot = async (name) => {
  await page.waitForTimeout(400);
  // Long pages (the scenario editor) otherwise photograph wherever the last
  // click left them.
  await page.evaluate(() => window.scrollTo(0, 0));
  await page.waitForTimeout(100);
  await page.screenshot({ path: `${out}/${name}.png` });
  console.log(`  ${name}.png`);
};

// The sequence below creates hx711_01 and the host server keeps its
// configuration on disk, so a second run would meet its own leftovers: since
// ?dry_run=1 now rejects a key that is already taken (as the real create
// always did), the Create button would legitimately stay disabled.  Start from
// a known state instead of depending on the order of runs.
const reset = await request.newContext({ baseURL: base });
// Milestone 11: the development server no longer waves requests through, so the
// script signs in exactly as an operator would.  Idempotent across runs: set the
// password the first time, use it thereafter.
const kPassword = 'bench-password';
const authState = await (await reset.get('/api/v1/auth')).json();
if (!authState.configured) {
  await reset.post('/api/v1/auth/password', { data: { password: kPassword } });
}
await reset.post('/api/v1/auth/login', { data: { password: kPassword } });
const before = await (await reset.get('/api/v1/devices')).json();
for (const device of before.devices ?? []) {
  if (device.key.startsWith('hx711_')) await reset.delete(`/api/v1/devices/${device.key}`);
}
// Milestone 12: the run now SEEDS the bench it needs instead of inheriting one
// from whoever ran the script last.  Every step below refers to channels by name
// — t_bath, heater — and those names came from devices created by hand in some
// earlier session and left in /tmp.  Against a fresh configuration directory the
// script failed five steps in, with a Playwright timeout that said nothing about
// the real cause: the bench did not exist.  That is the same invisible-prior-
// state problem this milestone spent its afternoon on, so it is fixed the same
// way — the run states what it needs and creates it.
//
// Channel KEYS are set explicitly through the per-channel overrides the devices
// API accepts, because "t_bath" is what the scenario, the interlock and the
// dataset all refer to; the generated "bath.value" would work equally well as a
// channel and would make every later step a search-and-replace.
const bench = [
  {
    key: 'bath',
    module: 'sim_signal',
    name: 'Water bath',
    // 55–65 degC: above the 40 degC the interlock is later moved to (so the
    // trip is immediate and not a coin toss on where the sine happens to be),
    // and around the 62 degC setpoint the loop is given, so the regulator on
    // screen looks like one.
    config: { waveform: 'sine', amplitude: 5, offset: 60, period_s: 90, unit: 'degC' },
    channels: { value: { key: 't_bath', name: 'Bath temperature', unit: 'degC', min: 0, max: 400 } },
  },
  {
    key: 'heat',
    module: 'heater',
    name: 'Bath heater',
    // max_duty 60: step 23 below commands 100 % and the tile has to answer 60,
    // which is the limit doing its job rather than the browser being polite.
    config: { pin: 25, frequency: 10, max_duty: 60, hold_s: 600 },
    channels: { power: { key: 'heater', name: 'Bath heater' } },
  },
];
const existing = new Set((before.devices ?? []).map((device) => device.key));
for (const device of bench) {
  if (existing.has(device.key)) continue;
  const created = await reset.post('/api/v1/devices', { data: device });
  if (!created.ok()) {
    throw new Error(`could not seed ${device.key}: ${created.status()} ${await created.text()}`);
  }
}

// The channel the UI-created load cell will expose.  The device key is chosen
// by the firmware (hx711_01), and the channel key follows from it — spelling
// that out once beats four literals that quietly refer to a device somebody
// created by hand in 2026.
const kLoadCellChannel = 'hx711_01.mass';

// Same for calibrations: the milestone-5 sequence below creates its record, and
// a second run would otherwise be looking at somebody else's version history.
const stored = await (await reset.get('/api/v1/calibrations')).json();
for (const record of stored.calibrations ?? []) {
  if (record.active) await reset.post(`/api/v1/calibrations/${encodeURIComponent(record.id)}/deactivate`, { data: {} });
  await reset.delete(`/api/v1/calibrations/${encodeURIComponent(record.id)}`);
}
// Dashboards too: the milestone-6 sequence builds one, and it must build it
// from nothing rather than inherit the previous run's layout.
const boards = await (await reset.get('/api/v1/dashboards')).json();
for (const board of boards.dashboards ?? []) {
  await reset.delete(`/api/v1/dashboards/${encodeURIComponent(board.key)}`);
}
// And the control configuration: loops and interlocks from a previous run would
// otherwise still be latched, still holding the outputs down, and the sequence
// below would be starting from somebody else's trip.
// Clearing the control document REMOVES interlocks, so it is confirmed by the
// password now (ADR-0020) — the reset has to speak the same policy the UI does.
await reset.put('/api/v1/control',
                { data: { loops: [], rules: [], limits: [], password: kPassword } });
// Scenarios too — the sequence below writes one from scratch, and a leftover
// from a previous run would make "new" mean "edit".
const scenarios = await (await reset.get('/api/v1/experiments')).json();
if (scenarios.run?.state === 'RUNNING' || scenarios.run?.state === 'PAUSED') {
  await reset.post(`/api/v1/experiments/${scenarios.run.experiment}/actions/stop`, { data: {} });
}
for (const scenario of scenarios.experiments ?? []) {
  await reset.delete(`/api/v1/experiments/${encodeURIComponent(scenario.key)}`);
}
await reset.post('/api/v1/outputs/clear', { data: {} });
await reset.dispose();

console.log('capturing:');
await page.goto(base, { waitUntil: 'networkidle' });

// The browser signs in for itself: the cookie the API context holds is not the
// one the page has.
await page.evaluate(async (password) => {
  await fetch('/api/v1/auth/login', {
    method: 'POST',
    headers: { 'Content-Type': 'application/json' },
    body: JSON.stringify({ password }),
    credentials: 'same-origin',
  });
}, kPassword);
await page.reload({ waitUntil: 'networkidle' });
await page.waitForSelector('.tile', { timeout: 5000 });
await shot('01-dashboard');

await page.getByRole('button', { name: 'Hardware' }).click();
await page.waitForSelector('table');
await shot('02-hardware');

// --- the acceptance sequence -------------------------------------------------
await page.getByRole('button', { name: 'Add device' }).click();
await page.waitForSelector('.dialog');
await shot('03-add-device-catalogue');

await page.locator('.card', { hasText: 'HX711' }).click();
await page.waitForSelector('.schema-form');

const selects = page.locator('.schema-form select');

/** Selects the option whose text starts with the given pin name. */
async function choosePin(select, pin) {
  const index = await select.evaluate((element, wanted) => {
    const options = Array.from(element.options);
    return options.findIndex((o) => o.textContent.trim().startsWith(wanted));
  }, pin);
  if (index < 0) throw new Error(`${pin} is not offered`);
  await select.selectOption({ index });
}

// The pin picker already prevents what it can: GPIO34 is greyed out for SCK
// because it has no output driver, and a pin owned by another device is
// unselectable.  What it CANNOT know is a cross-field rule — the same pin used
// twice inside one device.  That is the firmware's job, and this is it doing it.
await choosePin(selects.nth(0), 'GPIO18');
await choosePin(selects.nth(1), 'GPIO18');
await page.waitForTimeout(900);
await shot('04-add-device-validation-error');

await choosePin(selects.nth(1), 'GPIO19');
await page.waitForTimeout(900);
await shot('05-add-device-valid');

await page.getByRole('button', { name: 'Create' }).click();
await page.waitForSelector('.dialog', { state: 'detached', timeout: 5000 });
await page.waitForTimeout(600);
await shot('06-hardware-after-create');

await page.getByRole('button', { name: 'Channels' }).click();
await page.waitForSelector('tbody tr');
await page.locator('tbody tr').first().click();
await shot('07-channels');

await page.getByRole('button', { name: 'System' }).click();
await page.waitForSelector('dl');
await shot('08-system');

await page.getByRole('button', { name: 'Diagnostics' }).click();
await page.waitForTimeout(1200);
await shot('09-diagnostics');

await page.getByRole('button', { name: 'Dashboard' }).click();
await page.waitForTimeout(1500);
await shot('10-dashboard-with-hx711');

// --- Milestone 5: calibrate a load cell from three weights ------------------
await page.getByRole('button', { name: 'Channels' }).click();
await page.waitForSelector('tbody tr');
await shot('12-channels-uncalibrated');

await page.locator('tbody tr', { hasText: kLoadCellChannel })
          .getByRole('button', { name: 'Calibrate' }).click();
await page.waitForSelector('.dialog[aria-label="Calibrate channel"]');

// Three weights.  The raw values are the ones a real HX711 produces; in the
// lab they arrive through the Capture button instead of being typed.
const WEIGHTS = [[453211, 0], [498322, 100], [543419, 200]];
await page.getByRole('button', { name: '+ add point' }).click();
const rows = page.locator('.dialog .points tbody tr');
for (let i = 0; i < WEIGHTS.length; ++i) {
  await rows.nth(i).locator('input[type=number]').nth(0).fill(String(WEIGHTS[i][0]));
  await rows.nth(i).locator('input[type=number]').nth(1).fill(String(WEIGHTS[i][1]));
}
await page.locator('.dialog input[placeholder="g"]').fill('g');
await page.locator('.dialog input[placeholder*="three weights"]').fill('three weights, 21 °C');
await page.waitForTimeout(900);
await shot('13-calibration-fit');

await page.getByRole('button', { name: 'Save and activate' }).click();
await page.waitForSelector('.versions', { timeout: 5000 });
await page.waitForTimeout(800);
await shot('14-calibration-versions');

await page.locator('.dialog .close').click();
await page.waitForTimeout(1800);
await shot('15-channels-in-grams');

// --- and edit its processing chain ------------------------------------------
await page.locator('tbody tr', { hasText: kLoadCellChannel })
          .getByRole('button', { name: 'Processing' }).click();
await page.waitForSelector('.dialog[aria-label="Processing pipeline"]');
await page.locator('.add select').selectOption('median');
await page.getByRole('button', { name: 'Add', exact: true }).click();
await page.waitForTimeout(300);
await page.getByRole('button', { name: 'Apply and save' }).click();
await page.waitForTimeout(900);
await shot('16-pipeline-editor');
await page.locator('.dialog .close').click();

await page.getByRole('button', { name: 'Dashboard' }).click();
await page.waitForTimeout(1200);

// --- Milestone 6: build a dashboard and prove it survives a reload ---------
await page.getByRole('button', { name: 'Dashboard' }).click();
// A controller that has never had a dashboard gets one seeded from the rig.
await page.waitForSelector('.surface .cell', { timeout: 5000 });
await page.waitForTimeout(600);
await shot('17-dashboard-seeded');

await page.getByRole('button', { name: 'Edit layout' }).click();
await page.getByRole('button', { name: 'Gauge', exact: true }).click();
await page.waitForTimeout(400);
await shot('18-dashboard-editing');

// Drag the new gauge up into the row of tiles; the chart below must give way.
const tiles = page.locator('.surface .cell');
const gauge = tiles.nth(await tiles.count() - 1);
const box = await gauge.boundingBox();
await page.mouse.move(box.x + box.width / 2, box.y + 18);
await page.mouse.down();
await page.mouse.move(box.x - 380, box.y - 220, { steps: 12 });
await page.waitForTimeout(150);
await shot('19-dashboard-dragging');
await page.mouse.up();
await page.waitForTimeout(400);

const layoutBefore = await page.evaluate(() =>
  [...document.querySelectorAll('.surface .cell')].map((c) => getComputedStyle(c).gridArea));

// The layout is written 2 s after the LAST change, not on every snap (R4).
// Wait for the indicator rather than for a stopwatch: "saved" is the claim
// being tested, and a timeout that is merely long enough hides a save that
// never happened.
await page.waitForFunction(
  () => document.querySelector('.state')?.textContent?.trim() === 'saved',
  { timeout: 15000 });
await shot('20-dashboard-saved');

await page.reload({ waitUntil: 'networkidle' });
await page.waitForSelector('.surface .cell');
await page.waitForTimeout(700);
const layoutAfter = await page.evaluate(() =>
  [...document.querySelectorAll('.surface .cell')].map((c) => getComputedStyle(c).gridArea));
if (JSON.stringify(layoutBefore) !== JSON.stringify(layoutAfter)) {
  throw new Error(`the layout did not survive a reload:\n  ${JSON.stringify(layoutBefore)}\n  ${JSON.stringify(layoutAfter)}`);
}
console.log('  layout survived the reload');
await shot('21-dashboard-after-reload');

// --- Milestone 7: an output, its deadline, and the master stop -------------
await page.getByRole('button', { name: 'Edit layout' }).click();
await page.getByRole('button', { name: 'Output', exact: true }).click();
await page.waitForTimeout(300);
// Point it at the heater.
const channelPick = page.locator('.panel select').first();
const heaterIndex = await channelPick.evaluate((el) =>
  Array.from(el.options).findIndex((o) => o.textContent.includes('Bath heater')));
if (heaterIndex < 0) throw new Error('the heater channel is not offered');
await channelPick.selectOption({ index: heaterIndex });
await page.waitForTimeout(400);
await page.getByRole('button', { name: 'Done' }).click();
await page.waitForTimeout(600);
await shot('22-dashboard-with-control');

// Command it past its power limit: the tile has to show 60, not 100.
await page.locator('.control input[type=range]').first().evaluate((el) => {
  el.value = '100';
  el.dispatchEvent(new Event('change', { bubbles: true }));
});
await page.waitForTimeout(1200);
await shot('23-output-power-limited');

// The master stop is in the frame, reachable from every page.
await page.getByRole('button', { name: 'Stop all outputs' }).click();
await page.waitForTimeout(900);
await shot('24-outputs-stopped');
const bannerVisible = await page.locator('.tripped-banner').isVisible();
if (!bannerVisible) throw new Error('the trip left no banner on screen');
page.once('dialog', (d) => d.accept());
await page.getByRole('button', { name: /Outputs stopped/ }).click();
await page.waitForTimeout(900);

// --- Milestone 8: a regulator, an interlock, and which one wins -----------
const nav = (label) => page.locator('aside nav button', { hasText: label });

await nav('Control').click();
await page.waitForSelector('.panel');
await shot('25-control-empty');

// An interlock first — that is the order the page teaches, and the order the
// firmware applies them in.
await page.getByRole('button', { name: 'Add interlock' }).click();
const limitRow = page.locator('.panel', { hasText: 'Interlocks' }).locator('tbody tr').first();
await limitRow.locator('input.key').fill('bath_overtemp');
await limitRow.locator('select').nth(0).selectOption('t_bath');
await limitRow.locator('input.num').nth(1).fill('300');

await page.getByRole('button', { name: 'Add loop' }).click();
const loopCard = page.locator('.loop').first();
await loopCard.locator('input.key').fill('bath');
await loopCard.locator('details summary').click();
await loopCard.locator('select').nth(0).selectOption('t_bath');
await loopCard.locator('select').nth(1).selectOption('heater');
await loopCard.locator('label:has-text("Kp") input').fill('8');
await loopCard.locator('label:has-text("Ki") input').fill('0.4');
await shot('26-control-editing');

await page.getByRole('button', { name: 'Apply configuration' }).click();
await page.waitForTimeout(1200);

// The setpoint is an operation, not a configuration change: it takes effect
// without stopping anything.
await loopCard.locator('.setpoint input').first().fill('62');
await loopCard.getByRole('button', { name: 'Set' }).click();
await page.waitForTimeout(400);
await loopCard.getByRole('button', { name: 'automatic' }).click();
await page.waitForTimeout(2500);
await shot('27-loop-running');

const runningState = await loopCard.locator('.pill').first().textContent();
if (!runningState.includes('RUNNING')) {
  throw new Error(`the loop did not start regulating: ${runningState}`);
}

// Now the interlock is moved BELOW the bath temperature: the loop is doing
// exactly what it was told, and it is about to be overruled by something that
// never asked it anything.
await limitRow.locator('input.num').nth(1).fill('40');
// Applying a configuration change while a loop is regulating asks first, and
// says what it is about to stop.  Accepting is part of the sequence.
page.once('dialog', (d) => d.accept());
await page.getByRole('button', { name: 'Apply configuration' }).click();
await page.waitForTimeout(1500);
await shot('28-interlock-latched');

if (!(await page.locator('.tripped-banner').isVisible())) {
  throw new Error('a tripped interlock left no banner on screen');
}

// The loop can still be switched to automatic.  It will be refused, and the
// refusal is what BLOCKED means — the loop is not off, it is overruled.
await loopCard.getByRole('button', { name: 'automatic' }).click();
await page.waitForTimeout(1500);
const blocked = await loopCard.locator('.pill').first().textContent();
if (!blocked.includes('BLOCKED')) {
  throw new Error(`a loop commanding into a trip should read BLOCKED, got ${blocked}`);
}
await shot('29-loop-blocked-by-interlock');

// And the master stop cannot be cleared while the interlock is latched.
page.once('dialog', (d) => d.accept());
await page.getByRole('button', { name: /Outputs stopped/ }).click();
await page.waitForTimeout(900);
if (!(await page.locator('.tripped-banner').isVisible())) {
  throw new Error('clearing the stop released a latched interlock');
}
await shot('30-stop-refuses-to-clear');

// Resetting the latch while the bath is still above 40 °C re-arms the limit,
// which promptly trips again.  That is the answer to "can I make it go away".
await page.getByRole('button', { name: 'Reset', exact: true }).click();
// Wait for the state to come BACK rather than sleeping a fixed time: the reset
// clears the latch for the fraction of a second before the next safety pass
// re-raises it, and a stopwatch here would be testing the poll interval.
await page.waitForFunction(
  () => [...document.querySelectorAll('.pill')].some((p) => p.textContent.includes('LATCHED')),
  { timeout: 10000 });
await shot('31-reset-retrips-immediately');

// Put it back properly: change the limit to something the rig is not violating,
// then clear the stop.  Re-applying the configuration re-installs the limit
// un-latched — but the outputs stay held until somebody says otherwise.
await limitRow.locator('input.num').nth(1).fill('300');
page.once('dialog', (d) => d.accept());
await page.getByRole('button', { name: 'Apply configuration' }).click();
await page.waitForTimeout(1500);
page.once('dialog', (d) => d.accept());
await page.getByRole('button', { name: /Outputs stopped/ }).click();
await page.waitForTimeout(900);
if (await page.locator('.tripped-banner').isVisible()) {
  throw new Error('the stop would not clear after the interlock was reset');
}
await shot('32-control-recovered');

// --- and the loop on the dashboard ---------------------------------------
await nav('Dashboard').click();
await page.waitForTimeout(800);
await page.getByRole('button', { name: 'Edit layout' }).click();
await page.getByRole('button', { name: 'Loop', exact: true }).click();
await page.waitForTimeout(400);
const loopPick = page.locator('.panel select').first();
await loopPick.selectOption('bath');
await page.waitForTimeout(300);
await page.getByRole('button', { name: 'Done' }).click();
await page.waitForTimeout(1200);
await shot('33-dashboard-with-loop');

// --- Milestone 9: a scenario, a run, and an interruption that says so -----
await nav('Experiments').click();
await page.waitForSelector('.panel');
await shot('34-experiments-empty');

await page.getByRole('button', { name: 'New', exact: true }).click();
const meta = page.locator('.meta input');
await meta.nth(0).fill('evaporation');
await meta.nth(1).fill('Evaporation at 60 C');
await meta.nth(2).fill('spec scenario, shortened');

// The scenario is assembled from a closed vocabulary of steps.  There is no
// text box anywhere in this editor, and that is the security property (§32).
const steps = page.locator('.steps li');
await steps.first().locator('input').fill('run started');   // MARK_EVENT

await page.locator('.add').getByRole('button', { name: 'Set', exact: true }).click();
await steps.nth(1).locator('select').first().selectOption('bath.setpoint');
await steps.nth(1).locator('input[type=number]').first().fill('60');

await page.locator('.add').getByRole('button', { name: 'Set', exact: true }).click();
await steps.nth(2).locator('select').first().selectOption('bath.mode');
await steps.nth(2).locator('select').nth(1).selectOption('automatic');

await page.locator('.add').getByRole('button', { name: 'Wait until' }).click();
await steps.nth(3).locator('select').nth(0).selectOption('t_bath');
await steps.nth(3).locator('select').nth(1).selectOption('>=');
await steps.nth(3).locator('input[type=number]').nth(0).fill('50');
await steps.nth(3).locator('input[type=number]').nth(1).fill('30');

await page.locator('.add').getByRole('button', { name: 'Mark event' }).click();
await steps.nth(4).locator('input').fill('steady state reached');

await page.locator('.add').getByRole('button', { name: 'Run for' }).click();
await steps.nth(5).locator('input[type=number]').fill('600');

await page.locator('.add').getByRole('button', { name: 'Stop', exact: true }).click();
await shot('35-scenario-editor');

// A wait with no deadline is the one thing this milestone refuses outright.
// The editor cannot express "no timeout", so the closest a person can get is
// zero — and the firmware names the field.
await steps.nth(3).locator('input[type=number]').nth(1).fill('0');
await page.getByRole('button', { name: 'Save scenario' }).click();
await page.waitForTimeout(700);
if (!(await page.locator('.bad-banner').isVisible())) {
  throw new Error('a wait with no deadline was accepted');
}
await shot('36-wait-without-deadline-refused');

await steps.nth(3).locator('input[type=number]').nth(1).fill('30');
await page.getByRole('button', { name: 'Save scenario' }).click();
await page.waitForTimeout(900);

// --- run it -------------------------------------------------------------
// The three panels are Run, Scenario and Runs, in that order; addressing them
// by position beats matching on text that CSS has uppercased.
const panels = page.locator('.page > .panel');
const runPanel = panels.nth(0);
await runPanel.locator('input').nth(0).fill('A. Mescheryakov');
await runPanel.locator('input').nth(1).fill('NaCl 5 %');
await page.getByRole('button', { name: /^Start/ }).click();
await page.waitForTimeout(3000);
await shot('37-run-in-progress');

const runPillState = await page.locator('.panel .pill').first().textContent();
if (!runPillState.includes('RUNNING')) {
  throw new Error(`the run did not start: ${runPillState}`);
}

// Interrupted at an arbitrary moment — the acceptance criterion.
await page.getByRole('button', { name: 'Stop run' }).click();
await page.waitForTimeout(1500);
await shot('38-run-stopped');

const outcome = await page.locator('.panel .pill').first().textContent();
if (!outcome.includes('ABORTED')) {
  throw new Error(`a stopped run must read ABORTED, got ${outcome}`);
}
// And the history has to say so too — a half-run that reads as complete is
// worse than no record at all.  The record is written by a background task, so
// wait for it to appear rather than assuming it is already there.
await page.waitForFunction(
  () => {
    const panel = document.querySelectorAll('.page > .panel')[2];
    return !!panel && panel.textContent.includes('ABORTED');
  },
  { timeout: 10000 });
const historyText = await panels.nth(2).textContent();
if (!historyText.includes('A. Mescheryakov')) {
  throw new Error('the run record does not carry the operator');
}
await shot('39-run-history');

// --- Milestone 10: the run records itself, and the file can be taken away --
await nav('Experiments').click();
await page.waitForTimeout(600);

// What the scenario records is part of the scenario; the steps say when.
await page.locator('.logging summary').click();
const loggingPanel = page.locator('.logging');
await loggingPanel.locator('input[type=number]').fill('5');
for (const key of ['t_bath', kLoadCellChannel]) {
  await loggingPanel.locator('label', { hasText: key }).locator('input[type=checkbox]').check();
}
await page.locator('.add').getByRole('button', { name: 'Mark event' }).click();
await shot('41-scenario-logging');

// Two steps that were deliberately refused in Milestone 9 and work now.
const stepsForLogging = page.locator('.steps li');
await stepsForLogging.last().locator('input').fill('recording window');
await page.getByRole('button', { name: 'Save scenario' }).click();
await page.waitForTimeout(900);

const savedLogging = await (await request.newContext({ baseURL: base }))
  .get('/api/v1/experiments/evaporation');
const scenario = await savedLogging.json();
if (!scenario.logging || scenario.logging.channels.length !== 2) {
  throw new Error('the scenario did not keep its recording settings');
}

// Record manually — the operator who is not running a scenario has the same
// dataset machinery available.
await nav('Logs').click();
await page.waitForSelector('.panel');
await shot('42-logs-empty');

await page.locator('.start input').nth(0).fill('bench recording');
await page.locator('.start input').nth(1).fill('A. Mescheryakov');
await page.locator('.start input').nth(2).fill('NaCl 5 %');
await page.locator('.start input[type=number]').fill('5');
for (const key of ['t_bath', kLoadCellChannel]) {
  await page.locator('.channels label', { hasText: key }).locator('input').check();
}
await page.getByRole('button', { name: 'Record', exact: true }).click();
await page.waitForTimeout(4000);
await shot('43-recording');

await page.getByRole('button', { name: 'Stop recording' }).click();
await page.waitForTimeout(1500);
await shot('44-datasets');

// The dataset is downloadable AS A FILE: the browser streams it, and what
// arrives is the CSV with its reproducibility header (§48).
const api = await request.newContext({ baseURL: base });
const listed = await (await api.get('/api/v1/logs')).json();
const dataset = listed.logs[0];
if (!dataset || dataset.rows < 10) {
  throw new Error(`the dataset is empty: ${JSON.stringify(dataset)}`);
}
const csv = await (await api.get(`/api/v1/logs/${dataset.id}/export.csv`)).text();
for (const line of ['# operator: A. Mescheryakov', '# sample: NaCl 5 %',
                    '# config_fingerprint: ', 't_ms,epoch_ms,', '# complete']) {
  if (!csv.includes(line)) {
    throw new Error(`the exported CSV is missing "${line}"`);
  }
}
const dataRows = csv.split('\n').filter((l) => l && !l.startsWith('#')).length - 1;
if (dataRows < dataset.rows - 1) {
  throw new Error(`the file has ${dataRows} rows, the index claims ${dataset.rows}`);
}
console.log(`  dataset ${dataset.id}: ${dataRows} rows, header intact`);
await api.dispose();

// --- and on the dashboard ------------------------------------------------
await nav('Dashboard').click();
await page.waitForTimeout(800);
await page.getByRole('button', { name: 'Edit layout' }).click();
await page.getByRole('button', { name: 'Experiment', exact: true }).click();
await page.waitForTimeout(400);
await page.getByRole('button', { name: 'Done' }).click();
await page.waitForTimeout(1000);
await shot('40-dashboard-with-run');

// --- Milestone 13: a chart that can be asked for a different span ----------
// The bug this closes is invisible in a screenshot: a dashboard left open all
// day used to slow to a crawl, and the operator read that as a hung controller.
// What IS checkable from here is the control that replaced the unusable window
// setting — three buttons, one of them active, and a stored default that comes
// back after a reload.
await page.getByRole('button', { name: 'Edit layout' }).click();
await page.getByRole('button', { name: 'Chart', exact: true }).click();
await page.waitForTimeout(400);

const kChartTitle = 'Range test';
await page.locator('.panel input[type=text]').first().fill(kChartTitle);
// Plot the bath: a chart with no channels shows a prompt, not a range bar.
const seriesPick = page.locator('.panel .row select');
const bathIndex = await seriesPick.evaluate((el) =>
  Array.from(el.options).findIndex((o) => o.textContent.includes('Bath temperature')));
if (bathIndex < 0) throw new Error('the bath channel is not offered to the chart');
await seriesPick.selectOption({ index: bathIndex });
await page.getByRole('button', { name: 'Add', exact: true }).click();
await page.waitForTimeout(300);

// The stored default is a choice of three now, not a free number the browser
// could not honour.
const rangePick = page.locator('.panel label.field select');
const offered = await rangePick.evaluate((el) => Array.from(el.options).map((o) => o.value));
if (JSON.stringify(offered) !== JSON.stringify(['0', '300', '600'])) {
  throw new Error(`the chart offers ranges it cannot draw: ${offered.join(', ')}`);
}
await rangePick.selectOption('600');
// The layout is written 2 s after the last change; wait for the claim, not for
// a stopwatch.  The indicator only exists while editing, so this is the moment.
await page.waitForFunction(
  () => document.querySelector('.state')?.textContent?.trim() === 'saved',
  { timeout: 15000 });
await page.getByRole('button', { name: 'Done' }).click();
await page.waitForTimeout(600);

const chartTile = page.locator('.surface .cell').filter({ hasText: kChartTitle });
const rangeButtons = chartTile.locator('.ranges button');
if (await rangeButtons.count() !== 3) {
  throw new Error(`a chart must offer three ranges, found ${await rangeButtons.count()}`);
}
const pressed = async () => {
  const active = await chartTile.locator('.ranges button[aria-pressed=true]').all();
  if (active.length !== 1) throw new Error(`${active.length} ranges are active at once`);
  return (await active[0].textContent()).trim();
};
// The stored window_s decides where the buttons start.
if (await pressed() !== '10 min') {
  throw new Error(`a stored window of 600 s must select 10 min, not ${await pressed()}`);
}
await shot('50-chart-ranges');

// Scoped to THIS tile: the seeded dashboard already carries a chart of its own,
// and a range bar found anywhere on the page would answer for the wrong one.
for (const label of ['5 min', 'All', '10 min']) {
  const at = Date.now();
  await chartTile.getByRole('button', { name: label, exact: true }).click();
  await page.waitForFunction(
    ({ want, title }) => {
      const tile = [...document.querySelectorAll('.surface .cell')]
        .find((cell) => cell.textContent.includes(title));
      return tile?.querySelector('.ranges button[aria-pressed=true]')
                 ?.textContent.trim() === want;
    },
    { want: label, title: kChartTitle }, { timeout: 2000 });
  const took = Date.now() - at;
  if (took > 1000) throw new Error(`switching to ${label} took ${took} ms`);
  console.log(`  range ${label} in ${took} ms`);
}
await chartTile.getByRole('button', { name: '5 min', exact: true }).click();
await page.waitForTimeout(400);
await shot('51-chart-last-5-minutes');

// And it has to have DRAWN something.  Every check above passes on a chart that
// renders three perfect buttons over an empty canvas — which is exactly what a
// wrongly shaped data array produces, silently, with no error anywhere.
const inked = await chartTile.locator('canvas').first().evaluate((canvas) => {
  const pixels = canvas.getContext('2d')
    .getImageData(0, 0, canvas.width, canvas.height).data;
  let n = 0;
  for (let i = 3; i < pixels.length; i += 4) if (pixels[i] > 0) ++n;
  return n;
});
if (inked < 1000) throw new Error(`the chart drew nothing: ${inked} painted pixels`);
console.log(`  the chart drew ${inked} pixels`);

// Pressing a button is a way of looking, not a change to the instrument.  Ask
// the firmware rather than the browser: three range changes must have written
// nothing, and the stored default must still be the ten minutes the editor
// chose.  A dashboard that rewrote flash every time somebody glanced at the
// last five minutes would wear the partition out for nothing.
const boardApi = await request.newContext({ baseURL: base });
const savedBoards = await (await boardApi.get('/api/v1/dashboards')).json();
let storedWindow;
for (const board of savedBoards.dashboards ?? []) {
  const document_ = await (await boardApi.get(
    `/api/v1/dashboards/${encodeURIComponent(board.key)}`)).json();
  for (const widget of document_.widgets ?? []) {
    if (widget.config?.title === kChartTitle) storedWindow = widget.config.window_s;
  }
}
await boardApi.dispose();
if (storedWindow !== 600) {
  throw new Error(`looking at a range changed what was stored: window_s=${storedWindow}`);
}
await page.reload({ waitUntil: 'networkidle' });
await page.waitForSelector('.surface .cell');
await page.waitForTimeout(900);
const restored = await page.locator('.surface .cell').filter({ hasText: kChartTitle })
  .locator('.ranges button[aria-pressed=true]').textContent();
if (restored.trim() !== '10 min') {
  throw new Error(`the stored range did not survive a reload: ${restored.trim()}`);
}
console.log('  the stored chart range survived the reload');
await shot('52-chart-range-after-reload');

// --- Milestone 14: recording onto this device -----------------------------
// The acceptance criteria are mostly about honesty under failure, so this
// scenario deliberately breaks the link in the middle: what must appear
// afterwards is a GAP, not a smooth line and a row count that pretends the
// outage never happened.
// The outage below has to be a REAL one: `setOffline` blocks new requests but
// leaves an established WebSocket alone, so it proved nothing.  Routing the
// socket gives the script a handle it can actually close, which is what the
// browser sees when a tablet walks out of Wi-Fi range.
let liveSocket = null;
await page.routeWebSocket(/\/ws\/live/, (ws) => {
  liveSocket = ws;
  ws.connectToServer();
});
await page.reload({ waitUntil: 'networkidle' });
await nav('Dashboard').click();
await page.waitForTimeout(1500);

await page.getByRole('button', { name: 'Record on this device' }).click();
await page.waitForSelector('[role=dialog]');
await page.waitForTimeout(400);

// The dashboard's own channels are pre-selected: the operator asked to record
// what they are looking at.
const preselected = await page.locator('[role=dialog] .channels input:checked').count();
if (preselected === 0) {
  throw new Error('the dialog did not seed the channels this dashboard shows');
}
// Record everything visible, once a second.
const boxes = page.locator('[role=dialog] .channels input[type=checkbox]');
for (let i = 0; i < await boxes.count(); ++i) {
  if (!(await boxes.nth(i).isChecked())) await boxes.nth(i).check();
}
await page.locator('[role=dialog] input[type=radio][value="1Hz"]').check();
await page.locator('[role=dialog] input[type=text]').nth(1).fill('Alexander');
await page.locator('[role=dialog] input[type=text]').nth(2).fill('TEOS-07');
await shot('53-local-recording-dialog');

await page.getByRole('button', { name: 'Start recording' }).click();
await page.waitForSelector('.bar .dot', { timeout: 5000 });
await page.waitForTimeout(4000);
await shot('54-local-recording-running');

// The recording must survive leaving the Dashboard: it is not a property of a
// view being mounted.
await nav('Hardware').click();
await page.waitForTimeout(2500);
if (!(await page.locator('.bar .dot').isVisible())) {
  throw new Error('the recording indicator vanished when the page changed');
}
await shot('55-recording-follows-the-operator');

// Now break the link.  Everything between here and the reconnect must read as
// a hole in the archive.
if (!liveSocket) throw new Error('the telemetry socket was never routed');
liveSocket.close();
await page.waitForTimeout(7000);
// live.ts reconnects with backoff; the new socket goes through the same route.
await page.waitForFunction(
  () => !document.querySelector('.bar .warn')?.textContent?.includes('link down'),
  { timeout: 20000 });
await page.waitForTimeout(4000);

await nav('Dashboard').click();
await page.waitForTimeout(500);
const gapsText = await page.locator('.bar .numbers').first().textContent();
if (!/gaps: [1-9]/.test(gapsText ?? '')) {
  throw new Error(`losing the link did not produce a gap: "${gapsText?.trim()}"`);
}
console.log(`  the outage was recorded: ${gapsText.trim()}`);

// A manual mark, the thing an operator writes down at the bench.
await page.getByRole('button', { name: 'Mark event' }).click();
await page.locator('.bar input[type=text]').fill('added solvent');
await page.getByRole('button', { name: 'Save', exact: true }).click();
await page.waitForTimeout(600);

await page.getByRole('button', { name: 'Stop', exact: true }).click();
await page.waitForTimeout(1500);

// --- Local data -----------------------------------------------------------
await nav('Local data').click();
await page.waitForSelector('table tbody tr', { timeout: 5000 });
await page.waitForTimeout(600);

const row = page.locator('table tbody tr').first();
const rowText = (await row.textContent()) ?? '';
if (!/COMPLETE/.test(rowText)) {
  throw new Error(`the finished set is not COMPLETE: ${rowText.replace(/\s+/g, ' ')}`);
}
const storedGaps = Number((await row.locator('td').nth(5).textContent())?.trim());
if (!(storedGaps >= 1)) throw new Error(`the stored set claims ${storedGaps} gaps`);
const storedRows = Number((((await row.locator('td').nth(4).textContent()) ?? '')
  .trim().split(/\s+/)[0] ?? '').replace(/[^0-9]/g, ''));
if (!(storedRows > 3)) throw new Error(`the stored set has ${storedRows} rows`);
console.log(`  stored locally: ${storedRows} rows, ${storedGaps} gap(s)`);
await shot('56-local-data');

// Open it: the historical chart reads from IndexedDB, never from the socket.
await row.locator('button.link').click();
await page.waitForSelector('.uplot', { timeout: 5000 });
await page.waitForTimeout(1200);
const emptyNote = await page.locator('.wrap p.small').first().textContent();
if (!/empty buckets/.test(emptyNote ?? '')) {
  throw new Error(`the gap is not visible in the stored view: "${emptyNote?.trim()}"`);
}
await shot('57-local-session-chart');

// Export.  The CSV must carry the reproducibility header and the gap count.
const csvDownload = page.waitForEvent('download', { timeout: 20000 });
await row.locator('button', { hasText: 'CSV' }).click();
const csvFile = await csvDownload;
const csvPath = await csvFile.path();
const csvText = readFileSync(csvPath, 'utf8');
for (const needle of ['# source: client IndexedDB', '# controller_id: lc-',
                      '# operator: Alexander', '# sample: TEOS-07',
                      'client_epoch_ms,client_iso,device_ms']) {
  if (!csvText.includes(needle)) {
    throw new Error(`the exported CSV is missing "${needle}"`);
  }
}
const dataLines = csvText.split('\n').filter((line) => line && !line.startsWith('#'));
if (dataLines.length < 4) throw new Error('the exported CSV has no rows');
console.log(`  exported ${dataLines.length - 1} CSV rows with the header intact`);

// And the ZIP, which carries the events alongside the data.
const zipDownload = page.waitForEvent('download', { timeout: 20000 });
await row.locator('button', { hasText: 'ZIP' }).click();
const zipFile = await zipDownload;
const zipBytes = readFileSync(await zipFile.path());
if (zipBytes.readUInt32LE(0) !== 0x04034b50) {
  throw new Error('the exported ZIP does not start with a local file header');
}
if (!zipBytes.includes(Buffer.from('events.csv'))) {
  throw new Error('the exported ZIP has no events.csv');
}
console.log(`  exported a ${zipBytes.length}-byte ZIP containing events.csv`);

// Deleting takes the name typed out: this is the only copy unless it was saved.
await row.locator('button', { hasText: 'Delete' }).click();
await page.waitForSelector('.danger-panel');
const deleteButton = page.locator('.danger-panel button', { hasText: 'Delete' });
if (await deleteButton.isEnabled()) {
  throw new Error('delete was possible without confirming the name');
}
await shot('58-local-delete-confirm');
const sessionName = (await page.locator('.danger-panel strong').textContent() ?? '')
  .replace(/^Delete “/, '').replace(/”\?$/, '');
await page.locator('.danger-panel input').fill(sessionName);
await deleteButton.click();
await page.waitForTimeout(1000);
console.log('  the set was deleted only after its name was typed');

// --- Milestone 11: the lock that never locks the stop button --------------
await nav('System').click();
await page.waitForSelector('.panel');
await shot('45-access');

// Signed out, the instrument stays readable and stoppable.
await page.evaluate(async () => {
  await fetch('/api/v1/auth/logout', { method: 'POST', credentials: 'same-origin' });
});
await page.reload({ waitUntil: 'networkidle' });
await page.waitForTimeout(1200);
await shot('46-signed-out');

if (!(await page.locator('.signin').isVisible())) {
  throw new Error('a signed-out browser should be offered a sign-in');
}
// The stop button is still there, and still works.
const stopVisible = await page.getByRole('button', { name: /Stop all outputs|Outputs stopped/ }).isVisible();
if (!stopVisible) throw new Error('the stop button disappeared for a signed-out browser');

const anonymous = await request.newContext({ baseURL: base });
const stopped = await anonymous.post('/api/v1/outputs/trip', { data: {} });
if (stopped.status() !== 200) {
  throw new Error(`the emergency stop asked for a password: ${stopped.status()}`);
}
const cleared = await anonymous.post('/api/v1/outputs/clear', { data: {} });
if (cleared.status() !== 401) {
  throw new Error(`clearing a stop must require signing in, got ${cleared.status()}`);
}
const exported = await (await anonymous.get('/api/v1/config/export')).text();
if (exported.includes(kPassword) || exported.includes('"hash"')) {
  throw new Error('the export contains credential material');
}
await anonymous.dispose();
await shot('47-stopped-while-signed-out');

// Sign back in and put the rig back the way it was.
await page.evaluate(async (password) => {
  await fetch('/api/v1/auth/login', {
    method: 'POST',
    headers: { 'Content-Type': 'application/json' },
    body: JSON.stringify({ password }),
    credentials: 'same-origin',
  });
}, kPassword);
await page.reload({ waitUntil: 'networkidle' });
await page.waitForTimeout(800);
page.once('dialog', (d) => d.accept());
await page.getByRole('button', { name: /Outputs stopped/ }).click();
await page.waitForTimeout(800);
await shot('48-signed-in-again');

// Phone-sized: everything an operator needs at the bench must fit.
await page.setViewportSize({ width: 420, height: 860 });
await page.waitForTimeout(600);
await shot('11-dashboard-mobile');

await browser.close();
console.log('done');
