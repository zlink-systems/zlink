import http from 'node:http';
import fs from 'node:fs';
import path from 'node:path';
import process from 'node:process';
import { fileURLToPath } from 'node:url';
import { build } from 'esbuild';
import { listenOnBrowserSafeLoopbackPort } from './browser-safe-listen.mjs';

const scriptDir = path.dirname(fileURLToPath(import.meta.url));
const workspaceRoot = path.resolve(scriptDir, '../..');
const sampleName = process.argv[2];
if (!sampleName) {
  throw new Error('Usage: node scripts/browser-e2e/run-sample.mjs <Sample.Ts> --config <path>');
}
const configPath = readOption('--config');
if (!configPath) throw new Error('--config <path> is required.');

process.env.PLAYWRIGHT_BROWSERS_PATH ??= path.join(workspaceRoot, '.cache/ms-playwright');
const { chromium } = await import('playwright');
const definition = loadDefinition(configPath);
const sampleRoot = path.join(workspaceRoot, 'samples', sampleName);
const entryName = readOption('--entry') ?? 'main.ts';
const bundle = await build({
  entryPoints: [path.join(sampleRoot, 'Client', entryName)],
  bundle: true,
  write: false,
  format: 'esm',
  platform: 'browser',
  target: 'es2022',
  sourcemap: 'inline',
  logLevel: 'silent'
});

const server = http.createServer((request, response) => {
  void serve(request, response, bundle.outputFiles[0].contents, definition);
});
await listen(server);
const address = server.address();
if (typeof address === 'string' || address === null) throw new Error('Browser runner did not get a TCP port.');

let browser;
try {
  browser = await chromium.launch({ headless: true });
} catch (error) {
  throw new Error(
    `Chromium is not installed. Run 'npm run browser:install' first. ${error instanceof Error ? error.message : error}`
  );
}

const page = await browser.newPage();
page.on('console', (message) => process.stdout.write(`${message.text()}\n`));
page.on('pageerror', (error) => process.stderr.write(`${error.stack ?? error.message}\n`));

try {
  await page.goto(`http://127.0.0.1:${address.port}/`, { waitUntil: 'load' });
  await page.waitForFunction(
    () => window.__zlinkSampleResult?.status === 'passed' || window.__zlinkSampleResult?.status === 'failed',
    undefined,
    { timeout: definition.timeoutMs }
  );
  const result = await page.evaluate(() => window.__zlinkSampleResult);
  if (result?.status !== 'passed') {
    throw new Error(`${sampleName} browser scenario failed: ${result?.error ?? 'missing result'}`);
  }
} finally {
  await browser.close();
  await close(server);
}

function loadDefinition(filePath) {
  const value = JSON.parse(fs.readFileSync(filePath, 'utf8'));
  if (!Number.isFinite(value.timeoutMs) || value.timeoutMs <= 0) {
    throw new Error('Browser runner config requires a positive timeoutMs.');
  }
  if (value.config === null || typeof value.config !== 'object' || Array.isArray(value.config)) {
    throw new Error('Browser runner config requires a config object.');
  }
  if (!Array.isArray(value.proxies)) {
    throw new Error('Browser runner config requires a proxies array.');
  }
  for (const proxy of value.proxies) {
    if (typeof proxy?.prefix !== 'string' || typeof proxy?.target !== 'string') {
      throw new Error('Each browser proxy requires string prefix and target values.');
    }
  }
  return value;
}

async function serve(request, response, bundleBytes, definition) {
  const requestUrl = new URL(request.url ?? '/', 'http://runner.invalid');
  if (requestUrl.pathname === '/') {
    response.writeHead(200, { 'content-type': 'text/html; charset=utf-8', 'cache-control': 'no-store' });
    response.end('<!doctype html><meta charset="utf-8"><script type="module" src="/client.mjs"></script>');
    return;
  }
  if (requestUrl.pathname === '/client.mjs') {
    response.writeHead(200, { 'content-type': 'text/javascript; charset=utf-8', 'cache-control': 'no-store' });
    response.end(bundleBytes);
    return;
  }
  if (requestUrl.pathname === '/config.json') {
    response.writeHead(200, { 'content-type': 'application/json', 'cache-control': 'no-store' });
    response.end(JSON.stringify(definition.config));
    return;
  }
  const proxy = definition.proxies.find((candidate) =>
    requestUrl.pathname === candidate.prefix || requestUrl.pathname.startsWith(`${candidate.prefix}/`)
  );
  if (proxy) {
    await proxyRequest(request, response, requestUrl, proxy);
    return;
  }
  response.writeHead(404).end();
}

async function proxyRequest(request, response, requestUrl, proxy) {
  const target = new URL(proxy.target);
  const suffix = requestUrl.pathname.slice(proxy.prefix.length) || '/';
  const upstream = http.request({
    hostname: target.hostname,
    port: target.port,
    method: request.method,
    path: `${suffix}${requestUrl.search}`,
    headers: copyHeaders(request.headers)
  }, (upstreamResponse) => {
    response.writeHead(upstreamResponse.statusCode ?? 502, copyHeaders(upstreamResponse.headers));
    upstreamResponse.pipe(response);
  });
  upstream.on('error', (error) => {
    if (!response.headersSent) response.writeHead(502, { 'content-type': 'text/plain' });
    response.end(error.message);
  });
  request.pipe(upstream);
}

function copyHeaders(headers) {
  return Object.fromEntries(
    Object.entries(headers).filter(([name, value]) => value !== undefined && !['host', 'connection'].includes(name))
  );
}

function readOption(name) {
  const index = process.argv.indexOf(name);
  if (index < 0) return undefined;
  const value = process.argv[index + 1];
  if (!value || value.startsWith('--')) throw new Error(`${name} requires a value.`);
  return value;
}

function listen(server) {
  return listenOnBrowserSafeLoopbackPort(server);
}

function close(server) {
  return new Promise((resolve, reject) => server.close((error) => error ? reject(error) : resolve()));
}
