const assert = require('node:assert/strict');
const childProcess = require('node:child_process');
const http = require('node:http');
const path = require('node:path');
const test = require('node:test');
const { build } = require('esbuild');
const {
  closeBrowser,
  closeBrowserServer,
  closeContext,
  closeServer,
  stopChild
} = require('../support/bounded-cleanup');

const workspaceRoot = path.resolve(__dirname, '../..');
process.env.PLAYWRIGHT_BROWSERS_PATH ??= path.join(workspaceRoot, '.cache/ms-playwright');
const { chromium } = require('playwright');
const serverScript = path.join(__dirname, 'support/stream-server.js');
const certificate = path.join(workspaceRoot, 'test/fixtures/tls/server-cert.pem');
const key = path.join(workspaceRoot, 'test/fixtures/tls/server-key.pem');

test(
  'actual Chromium uses ws/wss, reconnect, drain, and browser trust',
  { timeout: 120_000 },
  async (t) => {
    let staticServer;
    let wsServer;
    let wssServer;
    let untrustedWssServer;
    let browserServer;
    let browser;
    let context;
    let secureContext;
    t.after(async () => {
      await cleanup(t, 'browser context', () => closeContext(context));
      await cleanup(t, 'secure browser context', () => closeContext(secureContext));
      await cleanup(t, 'browser', () => closeBrowser(browser));
      await cleanup(t, 'browser server', () => closeBrowserServer(browserServer));
      await cleanup(t, 'ws server', () => stopStreamServer(wsServer));
      await cleanup(t, 'wss server', () => stopStreamServer(wssServer));
      await cleanup(t, 'untrusted wss server', () => stopStreamServer(untrustedWssServer));
      await cleanup(t, 'static server', () => closeServer(staticServer?.server));
    });
    staticServer = await startStaticServer();
    wsServer = await startStreamServer('ws://127.0.0.1:0');
    const wsEndpoint = wsServer.endpoint;
    wssServer = await startStreamServer('wss://127.0.0.1:0', certificate, key);
    untrustedWssServer = await startStreamServer('wss://127.0.0.1:0', certificate, key);
    browserServer = await chromium.launchServer({ headless: true });
    browser = await chromium.connect({ wsEndpoint: browserServer.wsEndpoint() });
    context = await browser.newContext();
    const page = await context.newPage();
    secureContext = await browser.newContext({ ignoreHTTPSErrors: true });
    const securePage = await secureContext.newPage();
    const untrustedPage = await context.newPage();
    try {
      await Promise.all([
        page.goto(staticServer.url),
        securePage.goto(staticServer.url),
        untrustedPage.goto(staticServer.url)
      ]);
      await page.evaluate((endpoint) => window.browserConnectorTest.connect(endpoint), wsEndpoint);
      assert.deepEqual(await requestFromPage(page, 'plain-ws', wsServer), { value: 'plain-ws' });

      await stopStreamServer(wsServer);
      await page.waitForFunction(
        () => window.browserConnectorTest.state().connectionState !== 'connected',
        null,
        { timeout: 10_000 }
      );
      wsServer = await startStreamServer(wsEndpoint);
      await page.waitForFunction(
        () => window.browserConnectorTest.state().connectionState === 'connected',
        null,
        { timeout: 10_000 }
      );
      assert.deepEqual(await requestFromPage(page, 'after-reconnect', wsServer), {
        value: 'after-reconnect'
      });

      await assert.rejects(
        () =>
          untrustedPage.evaluate(
            (endpoint) => window.browserConnectorTest.connect(endpoint),
            untrustedWssServer.endpoint
          ),
        /connect|WebSocket|transport/i
      );
      assert.equal(untrustedWssServer.exitCode, null);
      await connectFromPage(securePage, wssServer.endpoint, wssServer);
      assert.deepEqual(await requestFromPage(securePage, 'secure-wss', wssServer), {
        value: 'secure-wss'
      });

      await stopStreamServer(wsServer);
      await page.waitForFunction(
        () => {
          const state = window.browserConnectorTest.state();
          return state.connectionState !== 'connected' && state.closeReason !== undefined;
        },
        null,
        { timeout: 10_000 }
      );
      const drained = await page.evaluate(() => window.browserConnectorTest.state());
      assert.notEqual(drained.closeReason, undefined);
    } finally {
      await Promise.allSettled([
        page.evaluate(() => window.browserConnectorTest.close()),
        securePage.evaluate(() => window.browserConnectorTest.close()),
        untrustedPage.evaluate(() => window.browserConnectorTest.close())
      ]);
    }
  }
);

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
    server
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
  child.stdout.on('data', (chunk) => {
    output += chunk;
  });
  child.stderr.on('data', (chunk) => {
    output += chunk;
  });
  return new Promise((resolve, reject) => {
    const timer = setTimeout(
      () => reject(new Error(`Stream server start timeout: ${output}`)),
      10_000
    );
    const check = () => {
      const readyLine = output
        .split('\n')
        .find((line) => line.includes('"event":"ready"') && line.endsWith('}'));
      if (!readyLine) return;
      clearTimeout(timer);
      child.stdout.off('data', check);
      child.endpoint = JSON.parse(readyLine).endpoint;
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

async function requestFromPage(page, value, server) {
  try {
    return await page.evaluate(
      (requestValue) => window.browserConnectorTest.request(requestValue),
      value
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
  return stopChild(child);
}

async function cleanup(t, resource, action) {
  try {
    const result = await action();
    if (result.timedOut)
      t.diagnostic(`${resource} cleanup exceeded its grace period; forced=${result.forced}`);
  } catch (error) {
    t.diagnostic(`${resource} cleanup failed: ${error.message}`);
  }
}
