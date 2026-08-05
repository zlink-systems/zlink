const assert = require('node:assert/strict');
const childProcess = require('node:child_process');
const http = require('node:http');
const net = require('node:net');
const path = require('node:path');
const test = require('node:test');
const { build } = require('esbuild');

const workspaceRoot = path.resolve(__dirname, '../..');
process.env.PLAYWRIGHT_BROWSERS_PATH ??= path.join(workspaceRoot, '.cache/ms-playwright');
const { chromium } = require('playwright');
const serverScript = path.join(__dirname, 'support/stream-server.js');
const certificate = path.join(workspaceRoot, 'test/fixtures/tls/server-cert.pem');
const key = path.join(workspaceRoot, 'test/fixtures/tls/server-key.pem');

test('actual Chromium uses ws/wss, explicit flow, reconnect, drain, and browser trust', { timeout: 120_000 }, async () => {
  const [wsPort, wssPort, untrustedWssPort] = await freePorts(3);
  const staticServer = await startStaticServer();
  let wsServer = await startStreamServer(`ws://127.0.0.1:${wsPort}`);
  const wssServer = await startStreamServer(`wss://127.0.0.1:${wssPort}`, certificate, key);
  const untrustedWssServer = await startStreamServer(
    `wss://127.0.0.1:${untrustedWssPort}`,
    certificate,
    key
  );
  const browser = await chromium.launch({ headless: true });
  const context = await browser.newContext();
  const page = await context.newPage();
  const secureContext = await browser.newContext({ ignoreHTTPSErrors: true });
  const securePage = await secureContext.newPage();
  const untrustedPage = await context.newPage();
  try {
    await Promise.all([
      page.goto(staticServer.url),
      securePage.goto(staticServer.url),
      untrustedPage.goto(staticServer.url)
    ]);
    await page.evaluate((endpoint) => window.browserConnectorTest.connect(endpoint), `ws://127.0.0.1:${wsPort}`);
    const explicitFlowId = '019f5c16-14f8-7701-9438-753e036a9b94';
    assert.deepEqual(await requestFromPage(page, 'plain-ws', wsServer, explicitFlowId), { value: 'plain-ws' });
    const flowState = await page.evaluate(() => window.browserConnectorTest.state());
    assert(flowState.observations.some((row) => row.flowId === explicitFlowId));

    await stopStreamServer(wsServer);
    await page.waitForFunction(() => window.browserConnectorTest.state().connectionState !== 'connected', null, { timeout: 10_000 });
    wsServer = await startStreamServer(`ws://127.0.0.1:${wsPort}`);
    await page.waitForFunction(() => window.browserConnectorTest.state().connectionState === 'connected', null, { timeout: 10_000 });
    assert.deepEqual(await requestFromPage(page, 'after-reconnect', wsServer), { value: 'after-reconnect' });

    await assert.rejects(
      () => untrustedPage.evaluate(
        (endpoint) => window.browserConnectorTest.connect(endpoint),
        `wss://127.0.0.1:${untrustedWssPort}`
      ),
      /connect|WebSocket|transport/i
    );
    assert.equal(untrustedWssServer.exitCode, null);
    await connectFromPage(securePage, `wss://127.0.0.1:${wssPort}`, wssServer);
    assert.deepEqual(await requestFromPage(securePage, 'secure-wss', wssServer), { value: 'secure-wss' });

    await stopStreamServer(wsServer);
    await page.waitForFunction(() => {
      const state = window.browserConnectorTest.state();
      return state.connectionState !== 'connected' && state.closeReason !== undefined;
    }, null, { timeout: 10_000 });
    const drained = await page.evaluate(() => window.browserConnectorTest.state());
    assert.notEqual(drained.closeReason, undefined);
  } finally {
    await Promise.allSettled([
      page.evaluate(() => window.browserConnectorTest.close()),
      securePage.evaluate(() => window.browserConnectorTest.close()),
      untrustedPage.evaluate(() => window.browserConnectorTest.close())
    ]);
    await browser.close();
    await Promise.allSettled([
      stopStreamServer(wsServer),
      stopStreamServer(wssServer),
      stopStreamServer(untrustedWssServer)
    ]);
    await staticServer.close();
  }
});

async function startStaticServer() {
  const output = await build({
    entryPoints: [path.join(__dirname, 'support/connector-scenario.ts')],
    bundle: true,
    write: false,
    format: 'esm',
    platform: 'browser',
    target: 'es2022'
  });
  const server = http.createServer((request, response) => {
    if (request.url === '/scenario.mjs') {
      response.writeHead(200, { 'content-type': 'text/javascript' });
      response.end(output.outputFiles[0].contents);
      return;
    }
    response.writeHead(200, { 'content-type': 'text/html' });
    response.end('<script type="module" src="/scenario.mjs"></script>');
  });
  await new Promise((resolve, reject) => {
    server.once('error', reject);
    server.listen(0, '127.0.0.1', resolve);
  });
  const address = server.address();
  return {
    url: `http://127.0.0.1:${address.port}`,
    close: () => new Promise((resolve) => server.close(resolve))
  };
}

function startStreamServer(endpoint, cert, privateKey) {
  const args = [serverScript, '--endpoint', endpoint];
  if (cert && privateKey) args.push('--certificate', cert, '--key', privateKey);
  const child = childProcess.spawn(process.execPath, args, {
    cwd: workspaceRoot,
    stdio: ['ignore', 'pipe', 'pipe']
  });
  let output = '';
  child.stdout.on('data', (chunk) => { output += chunk; });
  child.stderr.on('data', (chunk) => { output += chunk; });
  return new Promise((resolve, reject) => {
    const timer = setTimeout(() => reject(new Error(`Stream server start timeout: ${output}`)), 10_000);
    const check = () => {
      if (!output.includes('"event":"ready"')) return;
      clearTimeout(timer);
      child.stdout.off('data', check);
      child.capturedOutput = () => output;
      resolve(child);
    };
    child.stdout.on('data', check);
    child.once('exit', (code) => {
      if (!output.includes('"event":"ready"')) {
        clearTimeout(timer);
        reject(new Error(`Stream server exited ${code}: ${output}`));
      }
    });
  });
}

async function requestFromPage(page, value, server, explicitFlowId) {
  try {
    return await page.evaluate(
      ([requestValue, flow]) => window.browserConnectorTest.request(requestValue, flow),
      [value, explicitFlowId]
    );
  } catch (error) {
    error.message += `\nstream server output for '${value}':\n${server.capturedOutput?.() ?? '<unavailable>'}`;
    throw error;
  }
}

async function connectFromPage(page, endpoint, server) {
  try {
    await page.evaluate((target) => window.browserConnectorTest.connect(target), endpoint);
  } catch (error) {
    error.message += `\nstream server exit code: ${server.exitCode ?? '<running>'}`;
    error.message += `\nstream server output:\n${server.capturedOutput?.() ?? '<unavailable>'}`;
    throw error;
  }
}

async function stopStreamServer(child) {
  if (!child || child.exitCode !== null) return;
  child.kill('SIGTERM');
  await new Promise((resolve) => child.once('exit', resolve));
}

async function freePorts(count) {
  const servers = await Promise.all(Array.from({ length: count }, () => new Promise((resolve, reject) => {
    const server = net.createServer();
    server.once('error', reject);
    server.listen(0, '127.0.0.1', () => resolve(server));
  })));
  const ports = servers.map((server) => server.address().port);
  await Promise.all(servers.map((server) => new Promise((resolve) => server.close(resolve))));
  return ports;
}
