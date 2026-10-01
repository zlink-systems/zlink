const assert = require('node:assert/strict');
const test = require('node:test');
const { ZLinkHttpRequestBuilder } = require('../../packages/http-client/dist/request-builder');
const { ZLinkFrameworkErrorKind } = require('../../packages/framework/dist');

for (const mode of ['submitRaw', 'submit', 'download']) {
  for (const requestFails of [false, true]) {
    test(`R12 ${mode} one-shot ${requestFails ? 'request' : 'close'} failure wins`, async () => {
      const requestError = new Error('request failed');
      const closeError = new Error('close failed');
      const client = {
        runtime: {
          executeAsync: async () => {
            if (requestFails) throw requestError;
            return { status: 200, headers: {}, body: '{}' };
          }
        },
        close: async () => {
          throw closeError;
        }
      };
      const builder = new ZLinkHttpRequestBuilder(undefined, 'GET', '/', {
        build: () => client,
        captureExecutionTurn: () => undefined
      });
      await assert.rejects(
        mode === 'download' ? builder.download(() => {}) : builder[mode](),
        (error) => error === (requestFails ? requestError : closeError)
      );
    });
  }
}

for (const response of [
  { status: 500, headers: {}, body: '{}' },
  { status: 200, headers: {}, body: '{' }
]) {
  test(`R12 typed response failure (${response.status}, ${response.body}) precedes close`, async () => {
    const closeError = new Error('close failed');
    let closeCount = 0;
    const builder = new ZLinkHttpRequestBuilder(undefined, 'GET', '/', {
      build: () => ({
        runtime: { executeAsync: async () => response },
        close: async () => {
          closeCount++;
          throw closeError;
        }
      }),
      captureExecutionTurn: () => undefined
    });
    await assert.rejects(
      builder.submit(),
      (error) =>
        error.kind ===
        (response.status === 500
          ? ZLinkFrameworkErrorKind.InternalFailure
          : ZLinkFrameworkErrorKind.ProtocolError)
    );
    assert.equal(closeCount, 1);
  });
}
