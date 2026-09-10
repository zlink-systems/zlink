import http from 'node:http';
import path from 'node:path';
import process from 'node:process';
import { fileURLToPath } from 'node:url';
import { build } from 'esbuild';
import { listenOnBrowserSafeLoopbackPort } from './browser-safe-listen.mjs';
import { closeBrowser, closeContext, closeServer } from '../../test/support/bounded-cleanup.js';

const scriptDir = path.dirname(fileURLToPath(import.meta.url));
const workspaceRoot = path.resolve(scriptDir, '../..');

async function cleanupResources(context, browser, server) {
  const errors = [];
  for (const [name, cleanup] of [
    ['context', () => closeContext(context)],
    ['browser', () => closeBrowser(browser)],
    ['server', () => closeServer(server)]
  ]) {
    try {
      await cleanup();
    } catch (error) {
      errors.push(new Error(`${name} cleanup failed`, { cause: error }));
    }
  }
  return errors;
}

async function createBrowserConnectorDriver(options = {}) {
  process.env.PLAYWRIGHT_BROWSERS_PATH ??= path.join(workspaceRoot, '.cache/ms-playwright');
  const [{ chromium }, output] = await Promise.all([
    import('playwright'),
    build({
      entryPoints: [path.join(scriptDir, 'connector-client.ts')],
      bundle: true,
      write: false,
      format: 'esm',
      platform: 'browser',
      target: 'es2022'
    })
  ]);
  let server;
  let browser;
  let context;
  try {
    server = http.createServer((request, response) => {
      if (request.url === '/client.mjs') {
        response.writeHead(200, { 'content-type': 'text/javascript' });
        response.end(output.outputFiles[0].contents);
        return;
      }
      response.writeHead(200, { 'content-type': 'text/html' });
      response.end('<script type="module" src="/client.mjs"></script>');
    });
    await listenOnBrowserSafeLoopbackPort(server);
    browser = await chromium.launch({ headless: true });
    context = await browser.newContext({ ignoreHTTPSErrors: options.ignoreHTTPSErrors === true });
    const page = await context.newPage();
    const address = server.address();
    await page.goto(`http://127.0.0.1:${address.port}`);
    await page.waitForFunction(() => window.zlinkBrowserConnector !== undefined);
    return {
      connect: (endpoint, reconnect = false) => page.evaluate(
        ([value, enabled]) => window.zlinkBrowserConnector.connect(value, enabled),
        [endpoint, reconnect]
      ),
      request: (packetName, payload, compress = false) => page.evaluate(
        ([name, value, compressed]) => window.zlinkBrowserConnector.request(name, value, compressed),
        [packetName, payload, compress]
      ),
      state: () => page.evaluate(() => window.zlinkBrowserConnector.state()),
      waitForCloseReason: async (reason, timeoutMs = 3000) => {
        await page.waitForFunction(
          (expected) => window.zlinkBrowserConnector.state().closeReason === expected,
          reason,
          { timeout: timeoutMs }
        );
      },
      close: async () => {
        await page.evaluate(() => window.zlinkBrowserConnector.close()).catch(() => undefined);
        const errors = await cleanupResources(context, browser, server);
        context = undefined;
        browser = undefined;
        server = undefined;
        if (errors.length > 0) throw new AggregateError(errors, 'browser connector cleanup failed');
      }
    };
  } catch (error) {
    const cleanupErrors = await cleanupResources(context, browser, server);
    if (cleanupErrors.length > 0) error.cleanupErrors = cleanupErrors;
    throw error;
  }
}

export { createBrowserConnectorDriver };
