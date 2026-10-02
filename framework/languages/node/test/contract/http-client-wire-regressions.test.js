/* SPDX-License-Identifier: Apache-2.0 */
'use strict';

const test = require('node:test');
const assert = require('node:assert/strict');
const http = require('node:http');
const { ZLinkHttpClient } = require('../../packages/http-client/dist');
const { ZLinkFrameworkErrorKind } = require('../../packages/framework/dist');

test('browser HTTP body limit counts received bytes before UTF-8 decoding', async () => {
  const browser = await import('../../packages/http-client/dist/browser/index.mjs');
  const originalFetch = globalThis.fetch;
  globalThis.fetch = async () => new Response(Uint8Array.of(255));
  const client = browser.ZLinkHttpClient.create('http://localhost').maxResponseBodySize(1).build();
  try {
    const response = await client.get('/raw-byte').submitRaw();
    assert.equal(response.body, '\uFFFD');
  } finally {
    await client.close();
    globalThis.fetch = originalFetch;
  }
});

test('browser HTTP body overflow cancels the byte stream', async () => {
  const browser = await import('../../packages/http-client/dist/browser/index.mjs');
  const originalFetch = globalThis.fetch;
  let cancelled = false;
  let emitted = 0;
  globalThis.fetch = async () =>
    new Response(
      new ReadableStream(
        {
          pull(controller) {
            if (emitted++ < 2) controller.enqueue(Uint8Array.of(1, 2));
            else controller.close();
          },
          cancel() {
            cancelled = true;
          }
        },
        { highWaterMark: 0 }
      )
    );
  const client = browser.ZLinkHttpClient.create('http://localhost')
    .maxResponseBodySize(3)
    .timeout(25)
    .build();
  try {
    await assert.rejects(
      client.get('/overflow').submitRaw(),
      (error) => error.kind === ZLinkFrameworkErrorKind.Rejected
    );
    assert.equal(cancelled, true);
  } finally {
    await client.close();
    globalThis.fetch = originalFetch;
  }
});

test('browser HTTP buffered decoding preserves UTF-8 across byte chunks at the limit', async () => {
  const browser = await import('../../packages/http-client/dist/browser/index.mjs');
  const originalFetch = globalThis.fetch;
  globalThis.fetch = async () =>
    new Response(
      new ReadableStream({
        start(controller) {
          controller.enqueue(Uint8Array.of(0xe2));
          controller.enqueue(Uint8Array.of(0x82, 0xac, 0x21));
          controller.close();
        }
      })
    );
  const client = browser.ZLinkHttpClient.create('http://localhost').maxResponseBodySize(4).build();
  try {
    const response = await client.get('/utf8-chunks').submitRaw();
    assert.equal(response.body, '€!');
  } finally {
    await client.close();
    globalThis.fetch = originalFetch;
  }
});

test('HTTP timeout preserves the contracted TimeoutError cause', async () => {
  const server = http.createServer();
  await new Promise((resolve) => server.listen(0, '127.0.0.1', resolve));
  const client = ZLinkHttpClient.create(`http://127.0.0.1:${server.address().port}`)
    .timeout(25)
    .build();
  try {
    await assert.rejects(client.get('/timeout').submitRaw(), (error) => {
      assert.equal(error.kind, ZLinkFrameworkErrorKind.DeadlineExceeded);
      assert.equal(error.cause.name, 'TimeoutError');
      return true;
    });
  } finally {
    await client.close();
    server.closeAllConnections();
    await new Promise((resolve) => server.close(resolve));
  }
});
