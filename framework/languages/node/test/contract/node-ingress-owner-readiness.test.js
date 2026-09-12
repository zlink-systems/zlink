'use strict';

const assert = require('node:assert/strict');
const test = require('node:test');
const { ServiceMailbox } = require('../../packages/framework/dist/runtime/foundation/service-mailbox');

test('mailbox wakes on a newly ready owner, then again only when its held claim is released', () => {
  const wakes = [];
  const mailbox = new ServiceMailbox(domain => wakes.push(domain));
  const enqueue = owner => mailbox.tryEnqueue({ owner, domain: 'application', parts: [Buffer.from('x')] });
  try {
    for (let i = 0; i < 64; i++) assert.equal(enqueue('first'), true);
    assert.deepEqual(wakes, ['application']);
    const claim = mailbox.tryClaim('application', 64, 1024);
    assert.equal(claim.records.length, 64);
    assert.equal(enqueue('first'), true);
    assert.deepEqual(wakes, ['application'], 'a held owner cannot be dispatched yet');
    assert.equal(enqueue('second'), true);
    assert.deepEqual(wakes, ['application', 'application']);
    mailbox.release(claim);
    assert.deepEqual(wakes, ['application', 'application', 'application']);
    const second = mailbox.tryClaim('application', 64, 1024);
    assert.equal(second.owner, 'second');
    mailbox.release(second);
    const first = mailbox.tryClaim('application', 64, 1024);
    assert.equal(first.owner, 'first');
    mailbox.release(first);
    assert.equal(wakes.length, 3, 'empty owners need no wake');
  } finally {
    mailbox.close();
  }
});
