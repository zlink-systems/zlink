/* SPDX-License-Identifier: Elastic-2.0 */
'use strict';

const assert = require('node:assert/strict');
const test = require('node:test');
const { ZLinkFrameworkErrorKind } = require('../../packages/framework/dist');
const {
  ZLinkSpotSerialTurnExecutor
} = require('../../packages/framework/dist/runtime/spots/spot-serial-turn-executor');

for (const [instanceIntent, kind, message] of [
  [false, ZLinkFrameworkErrorKind.NotFound, 'SPOT is closing.'],
  [
    true,
    ZLinkFrameworkErrorKind.Unavailable,
    'The Instance Spot incarnation released its authority.'
  ]
]) {
  test(`Spot Close describes a refused request with instanceIntent=${instanceIntent}`, () => {
    const serial = new ZLinkSpotSerialTurnExecutor();
    assert.equal(serial.applicationAdmissionRefusal(instanceIntent), undefined);
    serial.closeAdmission();
    const refusal = serial.applicationAdmissionRefusal(instanceIntent);
    assert.equal(refusal.kind, kind);
    assert.equal(refusal.message, message);
  });
}
