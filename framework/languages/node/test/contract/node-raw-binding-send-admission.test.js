'use strict';

const assert = require('node:assert/strict');
const test = require('node:test');

const zlink = require('@zlink-systems/zlink');
const {
  ZLinkNodeRawBindingPort
} = require('../../packages/framework/dist/runtime/backend/node/node-raw-binding-port');

const OK_BURST_SIZE = 1024;
const MAX_UNFINISHED_DEPTH = 1;

test('raw binding port keeps consecutive OK send depth bounded', async t => {
  const neverAdmitted = new Promise(() => {});
  const createSocket = () => {
    const operation = {
      message() {
        return operation;
      },
      submit() {
        return { result: zlink.SubmitResult.Ok, admitted: neverAdmitted };
      }
    };
    return {
      options: {},
      send() {
        return operation;
      },
      close() {}
    };
  };
  t.mock.method(zlink, 'createRouterSocket', createSocket);
  t.mock.method(zlink, 'createDealerSocket', createSocket);

  const host = new ZLinkNodeRawBindingPort({}).createHost();
  const router = host.createRouter();
  const dealer = host.createDealer();
  t.after(() => host.close());

  await assertOkBurstDepthBounded(
    () => router.send('target', [Buffer.from('routed')]),
    'routed send'
  );
  await assertOkBurstDepthBounded(
    () => dealer.send([Buffer.from('dealer')]),
    'dealer send'
  );
});

async function assertOkBurstDepthBounded(send, label) {
  let unfinished = 0;
  let peakUnfinished = 0;
  const submissions = [];

  for (let index = 0; index < OK_BURST_SIZE; index++) {
    unfinished += 1;
    peakUnfinished = Math.max(peakUnfinished, unfinished);
    const submission = send().finally(() => {
      unfinished -= 1;
    });
    submissions.push(submission);

    // An OK submit has no admission work left. One microtask turn is enough for
    // the framework Promise to settle; awaiting submission.admitted would make
    // this unfinished depth grow on every iteration.
    await Promise.resolve();
  }

  assert.ok(
    peakUnfinished <= MAX_UNFINISHED_DEPTH,
    `${label} unfinished depth ${peakUnfinished} exceeded ${MAX_UNFINISHED_DEPTH}`
  );
  assert.equal(unfinished, 0, `${label} retained unfinished OK sends`);
  await Promise.all(submissions);
}
