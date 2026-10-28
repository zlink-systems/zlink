const assert = require('node:assert/strict');
const test = require('node:test');
const { defineZLinkPacketJsonContract } = require('../dist/internal/json');

test('enum schema rejects integer names and accepts declared text names', () => {
  for (const name of ['1', '-1', '0', '01']) {
    assert.throws(
      () => defineZLinkPacketJsonContract(`integer-enum-${name}`, {
        payload: { type: 'enum', names: [name] }
      }),
      /non-empty, non-numeric string names/
    );
  }

  assert.deepEqual(
    defineZLinkPacketJsonContract('text-enum', {
      payload: { type: 'enum', names: ['Ready'] }
    }).payload,
    { type: 'enum', names: ['Ready'] }
  );
});
