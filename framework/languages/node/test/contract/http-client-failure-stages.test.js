/* SPDX-License-Identifier: Apache-2.0 */
'use strict';

const test = require('node:test');
const assert = require('node:assert/strict');
const http = require('node:http');
const { ZLinkHttpClient } = require('../../packages/http-client/dist');
const { ZLinkFrameworkErrorKind } = require('../../packages/framework/dist');

for (const ErrorType of [Error, TypeError]) {
  for (const source of ['sink', 'provider']) {
    test(`Node HTTP ${source} ${ErrorType.name} is an application failure`, async () => {
      const server = http.createServer((request, response) => {
        request.on('error', () => {});
        request.resume();
        response.end('payload');
      });
      await new Promise((resolve) => server.listen(0, '127.0.0.1', resolve));
      const client = ZLinkHttpClient.create(`http://127.0.0.1:${server.address().port}`).build();
      const cause = new ErrorType(`${source} failed`);
      const callback = () => {
        throw cause;
      };
      try {
        const result =
          source === 'sink'
            ? client.get('/sink').download(callback)
            : client.post('/provider').bodyStream(callback, 'application/octet-stream').submitRaw();
        await assert.rejects(result, (error) => {
          assert.equal(error.kind, ZLinkFrameworkErrorKind.InternalFailure);
          assert.equal(error.cause, cause);
          return true;
        });
      } finally {
        await client.close();
        server.closeAllConnections();
        await new Promise((resolve) => server.close(resolve));
      }
    });

    test(`browser HTTP ${source} ${ErrorType.name} is an application failure`, async () => {
      const browser = await import('../../packages/http-client/dist/browser/index.mjs');
      const originalFetch = globalThis.fetch;
      globalThis.fetch = async (_url, init) => {
        if (init.body !== undefined) {
          const reader = init.body.getReader();
          try {
            await reader.read();
          } finally {
            reader.releaseLock();
          }
        }
        return new Response('payload');
      };
      const client = browser.ZLinkHttpClient.create('http://localhost').build();
      const cause = new ErrorType(`${source} failed`);
      const callback = () => {
        throw cause;
      };
      try {
        const result =
          source === 'sink'
            ? client.get('/sink').download(callback)
            : client.post('/provider').bodyStream(callback, 'application/octet-stream').submitRaw();
        await assert.rejects(result, (error) => {
          assert.equal(error.kind, ZLinkFrameworkErrorKind.InternalFailure);
          assert.equal(error.cause, cause);
          return true;
        });
      } finally {
        await client.close();
        globalThis.fetch = originalFetch;
      }
    });
  }
}

for (const source of ['fetch', 'response']) {
  test(`browser HTTP ${source} TypeError is a transport failure`, async () => {
    const browser = await import('../../packages/http-client/dist/browser/index.mjs');
    const originalFetch = globalThis.fetch;
    const cause = new TypeError('transport failed');
    globalThis.fetch = async () => {
      if (source === 'fetch') throw cause;
      return new Response(
        new ReadableStream({
          start(controller) {
            controller.error(cause);
          }
        })
      );
    };
    const client = browser.ZLinkHttpClient.create('http://localhost').build();
    try {
      await assert.rejects(client.get('/transport').submitRaw(), (error) => {
        assert.equal(error.kind, ZLinkFrameworkErrorKind.Unavailable);
        assert.equal(error.cause, cause);
        return true;
      });
    } finally {
      await client.close();
      globalThis.fetch = originalFetch;
    }
  });
}

test('Node HTTP response connection reset is a transport failure', async () => {
  const server = http.createServer((_request, response) => {
    response.writeHead(200, { 'Content-Length': 100 });
    response.write('partial', () => response.destroy());
  });
  await new Promise((resolve) => server.listen(0, '127.0.0.1', resolve));
  const client = ZLinkHttpClient.create(`http://127.0.0.1:${server.address().port}`).build();
  try {
    await assert.rejects(client.get('/reset').submitRaw(), (error) => {
      assert.equal(error.kind, ZLinkFrameworkErrorKind.Unavailable);
      return true;
    });
  } finally {
    await client.close();
    server.closeAllConnections();
    await new Promise((resolve) => server.close(resolve));
  }
});
