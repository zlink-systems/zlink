// SPDX-License-Identifier: MPL-2.0

import test from 'node:test';
import assert from 'node:assert/strict';

const { RoutingId } = require('@zlink-systems/zlink');

const displayCases: ReadonlyArray<readonly [string, string]> = [
  ['6465616c65722d31', 'dealer-1'],
  ['ed959ceab880', '한글'],
  ['f09f9880', '😀'],
  ['efbfbd', '�'],
  ['c2a0', '\u00a0'],
  ['c280', 'hex:c280'],
  ['c29f', 'hex:c29f'],
  ['7f', 'hex:7f'],
  ['00', 'hex:00'],
  ['1f', 'hex:1f'],
  ['80', 'hex:80'],
  ['c080', 'hex:c080'],
  ['eda080', 'hex:eda080'],
  ['e282', 'hex:e282'],
  ['f4908080', '4103110784'],
  ['00000017', '23'],
  ['000102030405060708090a0b0c0d0e0f', '00010203-0405-0607-0809-0a0b0c0d0e0f'],
];

test('routing id display preserves printable UTF-8 and binary fallback order', () => {
  for (const [hex, expected] of displayCases) {
    assert.equal(RoutingId.fromHex(hex).toString(), expected, hex);
  }
});

test('routing id display does not allocate byte storage to validate UTF-8', () => {
  const ids = displayCases.map(([hex]) => RoutingId.fromHex(hex));
  const originalFrom = Buffer.from;
  let allocations = 0;
  let bytes = 0;
  const actual: string[] = [];
  Buffer.from = ((...args: any[]) => {
    const buffer = Reflect.apply(originalFrom, Buffer, args);
    allocations += 1;
    bytes += buffer.length;
    return buffer;
  }) as typeof Buffer.from;
  try {
    for (const id of ids) actual.push(id.toString());
  } finally {
    Buffer.from = originalFrom;
  }
  assert.deepEqual(actual, displayCases.map(([, expected]) => expected));
  assert.deepEqual({ allocations, bytes }, { allocations: 0, bytes: 0 });
});

test('routing id byte access preserves independent ownership', () => {
  const input = Buffer.from('peer');
  const id = RoutingId.from(input);
  input.fill(0);
  const first = id.toBytes();
  const second = id.toBytes();
  first.fill(0);
  assert.equal(second.toString(), 'peer');
  assert.equal(id.toString(), 'peer');
  assert.notStrictEqual(first, second);
});
