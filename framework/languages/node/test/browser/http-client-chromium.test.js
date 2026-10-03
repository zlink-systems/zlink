/* SPDX-License-Identifier: Apache-2.0 */
'use strict';

const assert = require('node:assert/strict');
const fs = require('node:fs');
const http = require('node:http');
const path = require('node:path');
const test = require('node:test');
const {
  closeBrowser,
  closeBrowserServer,
  closeContext,
  closeServer
} = require('../support/bounded-cleanup');
const { ZLinkFrameworkErrorKind } = require('../../packages/framework/dist');

const workspaceRoot = path.resolve(__dirname, '../..');
process.env.PLAYWRIGHT_BROWSERS_PATH ??= path.join(workspaceRoot, '.cache/ms-playwright');
const { chromium } = require('playwright');
let server;
let browserServer;
let browser;
let context;
let page;

test.before(async () => {
  const bundle = fs.readFileSync(
    path.join(workspaceRoot, 'packages/http-client/dist/browser/index.mjs')
  );
  server = http.createServer((request, response) => {
    if (request.url === '/bundle.mjs') {
      response.writeHead(200, { 'Content-Type': 'text/javascript' });
      response.end(bundle);
    } else if (request.url === '/timeout') {
      request.resume();
    } else if (request.url === '/reset') {
      response.writeHead(200, { 'Content-Length': 100 });
      response.write('partial', () => response.destroy());
    } else {
      request.resume();
      response.end('payload');
    }
  });
  await new Promise((resolve) => server.listen(0, '127.0.0.1', resolve));
  browserServer = await chromium.launchServer({ headless: true });
  browser = await chromium.connect({ wsEndpoint: browserServer.wsEndpoint() });
  context = await browser.newContext();
  page = await context.newPage();
  await page.goto(`http://127.0.0.1:${server.address().port}/`);
});

test.after(async () => {
  const results = [
    await closeContext(context),
    await closeBrowser(browser),
    await closeBrowserServer(browserServer)
  ];
  server?.closeAllConnections();
  results.push(await closeServer(server));
  for (const result of results)
    assert.equal(result.timedOut, false, 'HTTP browser cleanup exceeded its bound');
});

for (const errorType of ['Error', 'TypeError']) {
  for (const source of ['provider', 'sink']) {
    test(`native Chromium HTTP ${source} ${errorType} preserves InternalFailure and cause`, async () => {
      const result = await page.evaluate(
        async ({ source, errorType }) => {
          const { ZLinkHttpClient } = await import('/bundle.mjs');
          const client = ZLinkHttpClient.create(location.origin).build();
          const cause =
            errorType === 'TypeError'
              ? new TypeError('callback failed')
              : new Error('callback failed');
          let called = false;
          const callback = () => {
            called = true;
            throw cause;
          };
          try {
            if (source === 'provider') {
              await client
                .post('/provider')
                .bodyStream(callback, 'application/octet-stream')
                .submitRaw();
            } else {
              await client.get('/sink').download(callback);
            }
            return { called, completed: true };
          } catch (error) {
            return { called, kind: error.kind, preserved: error.cause === cause };
          } finally {
            await client.close();
          }
        },
        { source, errorType }
      );
      assert.deepEqual(result, {
        called: true,
        kind: ZLinkFrameworkErrorKind.InternalFailure,
        preserved: true
      });
    });
  }
}

test('native Chromium HTTP response reset remains Unavailable', async () => {
  const kind = await page.evaluate(async () => {
    const { ZLinkHttpClient } = await import('/bundle.mjs');
    const client = ZLinkHttpClient.create(location.origin).build();
    try {
      await client.get('/reset').submitRaw();
      return undefined;
    } catch (error) {
      return error.kind;
    } finally {
      await client.close();
    }
  });
  assert.equal(kind, ZLinkFrameworkErrorKind.Unavailable);
});

test('native Chromium HTTP timeout retains DeadlineExceeded and TimeoutError cause', async () => {
  const result = await page.evaluate(async () => {
    const { ZLinkHttpClient } = await import('/bundle.mjs');
    const client = ZLinkHttpClient.create(location.origin).timeout(25).build();
    try {
      await client.get('/timeout').submitRaw();
      return undefined;
    } catch (error) {
      return { kind: error.kind, causeName: error.cause?.name };
    } finally {
      await client.close();
    }
  });
  assert.deepEqual(result, {
    kind: ZLinkFrameworkErrorKind.DeadlineExceeded,
    causeName: 'TimeoutError'
  });
});
