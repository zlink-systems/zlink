// Runs a built Unity WebGL player against a real STREAM server in Chromium.
//
//   node test/browser/support/unity-player/drive-player.js \
//     --player <dir> --label RuntimeSpeed --expect pass|known-failure \
//     [--expect-error <regexp>]
//
// test/browser/unity-webgl-emscripten.test.js covers the layer below this one:
// the jslib link, the reverse function pointer and the heap marshalling, with a
// C harness standing in for IL2CPP. This script is the layer above it and
// answers only what Unity adds - IL2CPP codegen, asmdef platform gating, UPM
// import and Unity's own .jspre/.jslib linking - so it drives the player and
// reads the one value the player reports.
//
// It is not a `node --test` file: it needs a Unity build directory that only CI
// produces, and the outcome it checks is whether the player ran, which the test
// runner has no way to skip on.
const fs = require('node:fs');
const http = require('node:http');
const net = require('node:net');
const path = require('node:path');
const childProcess = require('node:child_process');

const workspaceRoot = path.resolve(__dirname, '../../../..');
process.env.PLAYWRIGHT_BROWSERS_PATH ??= path.join(workspaceRoot, '.cache/ms-playwright');
const serverScript = path.join(__dirname, '../stream-server.js');

const CONTENT_TYPES = new Map([
  ['.html', 'text/html'],
  ['.js', 'text/javascript'],
  ['.json', 'application/json'],
  ['.wasm', 'application/wasm'],
  ['.data', 'application/octet-stream'],
  ['.symbols', 'application/octet-stream'],
  ['.css', 'text/css'],
  ['.png', 'image/png'],
  ['.jpg', 'image/jpeg'],
  ['.ico', 'image/x-icon']
]);

const PAGE_TIMEOUT_MS = Number(argument('--timeout') ?? 120_000);

main().catch((error) => {
  process.stderr.write(`${error.stack ?? error.message}\n`);
  process.exitCode = 1;
});

async function main() {
  const player = path.resolve(required('--player'));
  const label = argument('--label') ?? path.basename(player);
  const expect = argument('--expect') ?? 'pass';
  const expectedError = argument('--expect-error');
  if (expect !== 'pass' && expect !== 'known-failure') {
    throw new Error(`--expect must be pass or known-failure, got ${expect}`);
  }

  const observed = await run(player, label);
  report(label, observed);

  const outcomeFile = argument('--outcome');
  if (outcomeFile) fs.writeFileSync(outcomeFile, `${JSON.stringify({ label, expect, ...observed }, null, 2)}\n`);

  if (expect === 'pass') {
    if (observed.ok) return;
    throw new Error(`${label}: the player was expected to pass; ${observed.reason}`);
  }

  // A known failure that starts passing is a finding, not a pass: the reason the
  // adapter documents a build-setting restriction has gone away.
  if (observed.ok) {
    throw new Error(
      `${label}: the player was expected to fail and did not. The documented restriction on this ` +
      'optimization level no longer holds - re-check the adapter README before relaxing this expectation.'
    );
  }

  if (expectedError && !new RegExp(expectedError).test(observed.reason ?? '')) {
    throw new Error(
      `${label}: expected the known failure /${expectedError}/, got a different one: ${observed.reason}`
    );
  }

  process.stdout.write(`${label}: failed as expected (${observed.reason})\n`);
}

async function run(player, label) {
  const { chromium } = require('playwright');
  const index = findIndex(player);
  const streamPort = await freePort();
  const endpoint = `ws://127.0.0.1:${streamPort}`;

  const streamServer = await startStreamServer(endpoint);
  const staticServer = await startStaticServer(player, index);
  // Unity's player loop is requestAnimationFrame. Headless Chromium throttles
  // rAF for a renderer it considers backgrounded or occluded, and a throttled
  // player stops calling Update, stops pumping the connector, and looks exactly
  // like a boundary that went silent. These flags keep the loop running.
  const browser = await chromium.launch({
    headless: true,
    args: [
      '--disable-background-timer-throttling',
      '--disable-backgrounding-occluded-windows',
      '--disable-renderer-backgrounding',
      '--disable-features=CalculateNativeWinOcclusion'
    ]
  });
  const messages = [];
  const errors = [];

  try {
    const context = await browser.newContext();
    const page = await context.newPage();
    page.on('console', (message) => {
      const text = `${message.type()}: ${message.text()}`;
      messages.push(text);
      process.stdout.write(`# ${label} console ${text}\n`);
    });
    page.on('pageerror', (error) => {
      const text = error.message ?? String(error);
      errors.push(text);
      process.stdout.write(`# ${label} pageerror ${text}\n`);
    });

    const url = `${staticServer.url}/${index}?endpoint=${encodeURIComponent(endpoint)}`;
    process.stdout.write(`# ${label} opening ${url}\n`);
    await page.goto(url, { waitUntil: 'domcontentloaded' });

    let raw;
    try {
      raw = await page.waitForFunction(
        () => (typeof window.zlinkVerificationResult === 'string' ? window.zlinkVerificationResult : null),
        undefined,
        { timeout: PAGE_TIMEOUT_MS, polling: 250 }
      ).then((handle) => handle.jsonValue());
    } catch {
      // The player publishes a heartbeat from Update every frame. Reading it
      // here turns "it stopped" into "it stopped here, with the frame loop
      // still running / already stopped", which is the difference between one
      // more Unity build and none.
      const heartbeat = await page.evaluate(
        () => globalThis.zlinkVerificationHeartbeat ?? null
      ).catch(() => null);
      const linked = await page.evaluate(() => ({
        runtime: typeof globalThis.ZlinkStreamWebGlRuntime !== 'undefined',
        bundle: typeof globalThis.ZlinkStreamConnectorBundle !== 'undefined'
      })).catch(() => null);
      return {
        ok: false,
        reason: errors.length > 0
          ? `the player never reported; first page error: ${errors[0]}`
          : `the player never reported within ${PAGE_TIMEOUT_MS} ms; heartbeat ${heartbeat ?? 'absent'}`,
        heartbeat: heartbeat === null ? null : JSON.parse(heartbeat),
        linkedPlugins: linked,
        pageErrors: errors,
        console: messages.slice(-40)
      };
    }

    const result = JSON.parse(raw);
    return {
      ok: result.ok === true && errors.length === 0,
      reason: result.ok === true
        ? (errors.length > 0 ? `the player reported ok but the page raised: ${errors[0]}` : null)
        : `the player reported: ${result.reason}`,
      report: result,
      pageErrors: errors
    };
  } finally {
    await browser.close().catch(() => undefined);
    await closeServer(staticServer.server);
    await stopChild(streamServer);
  }
}

function report(label, observed) {
  const summary = observed.report;
  process.stdout.write(`# ${label} ok=${observed.ok}${observed.reason ? ` reason=${observed.reason}` : ''}\n`);
  if (summary) {
    process.stdout.write(
      `# ${label} unity=${summary.unityVersion} assembly=${summary.adapterAssembly} ` +
      `plugins=runtime:${summary.linkedPlugins?.runtime},bundle:${summary.linkedPlugins?.bundle}\n`
    );
    for (const step of summary.steps ?? []) process.stdout.write(`#   ${label} ${step}\n`);
  }
}

// Unity writes index.html at the root of the build folder, but a build placed
// inside another folder keeps its own name, so take whichever index.html sits
// next to a Build directory.
function findIndex(player) {
  if (fs.existsSync(path.join(player, 'index.html'))) return 'index.html';
  const nested = fs.readdirSync(player, { withFileTypes: true })
    .filter((entry) => entry.isDirectory())
    .map((entry) => path.join(entry.name, 'index.html'))
    .find((candidate) => fs.existsSync(path.join(player, candidate)));
  if (!nested) throw new Error(`no index.html under ${player}`);
  return nested.split(path.sep).join('/');
}

function startStaticServer(root, index) {
  const server = http.createServer((request, response) => {
    const requested = decodeURIComponent(new URL(request.url, 'http://127.0.0.1').pathname).replace(/^\/+/, '');
    const file = path.resolve(root, requested === '' ? index : requested);
    if (!file.startsWith(path.resolve(root)) || !fs.existsSync(file) || !fs.statSync(file).isFile()) {
      response.writeHead(404).end('not found');
      return;
    }

    const type = CONTENT_TYPES.get(path.extname(file)) ?? 'application/octet-stream';
    response.writeHead(200, { 'content-type': type });
    fs.createReadStream(file).pipe(response);
  });

  return new Promise((resolve, reject) => {
    server.on('error', reject);
    server.listen(0, '127.0.0.1', () => {
      resolve({ server, url: `http://127.0.0.1:${server.address().port}` });
    });
  });
}

// The same server the rest of the browser e2e uses, started the same way: the
// point is that the player talks to the real framework, not to a stub.
function startStreamServer(endpoint) {
  return new Promise((resolve, reject) => {
    const child = childProcess.spawn(process.execPath, [serverScript, '--endpoint', endpoint], {
      cwd: workspaceRoot,
      stdio: ['ignore', 'pipe', 'pipe']
    });
    let buffered = '';
    const timer = setTimeout(() => reject(new Error('the STREAM server did not become ready')), 60_000);
    child.stdout.setEncoding('utf8');
    child.stdout.on('data', (chunk) => {
      buffered += chunk;
      for (const line of buffered.split('\n')) {
        if (!line.trim().startsWith('{')) continue;
        try {
          if (JSON.parse(line).event === 'ready') {
            clearTimeout(timer);
            resolve(child);
            return;
          }
        } catch {
          // A partial line; the next chunk completes it.
        }
      }
    });
    child.stderr.setEncoding('utf8');
    child.stderr.on('data', (chunk) => process.stderr.write(`# stream-server ${chunk}`));
    child.on('error', reject);
    child.on('exit', (code) => {
      clearTimeout(timer);
      reject(new Error(`the STREAM server exited with ${code}`));
    });
  });
}

function stopChild(child) {
  if (!child || child.exitCode !== null) return Promise.resolve();
  return new Promise((resolve) => {
    child.removeAllListeners('exit');
    child.once('exit', () => resolve());
    child.kill('SIGTERM');
    setTimeout(() => { child.kill('SIGKILL'); resolve(); }, 5_000).unref();
  });
}

function closeServer(server) {
  return new Promise((resolve) => (server ? server.close(() => resolve()) : resolve()));
}

function freePort() {
  return new Promise((resolve, reject) => {
    const probe = net.createServer();
    probe.on('error', reject);
    probe.listen(0, '127.0.0.1', () => {
      const { port } = probe.address();
      probe.close(() => resolve(port));
    });
  });
}

function argument(name) {
  const index = process.argv.indexOf(name);
  return index >= 0 ? process.argv[index + 1] : undefined;
}

function required(name) {
  const value = argument(name);
  if (!value) throw new Error(`${name} is required`);
  return value;
}
