'use strict';

const assert = require('node:assert/strict');
const test = require('node:test');
const path = require('node:path');
const { buildCases, decodeHeader } = require('../helpers/channel-header-wire-cases');

// test/fixtures/channel-header-wire-golden.json was captured from the origin/main
// build with test/helpers/channel-header-wire-cases.js. Never regenerate it from
// the code under test: a change in any encoded header or frame byte must fail.
const golden = require('../fixtures/channel-header-wire-golden.json');
const dist = path.resolve(__dirname, '../../packages/framework/dist');
const envelope = require(`${dist}/runtime/channels/channel-envelope`);
const actual = buildCases(dist);

test('channel header, application frame and request header bytes equal origin/main', () => {
  assert.equal(actual.cases.length, golden.cases.length);
  for (let index = 0; index < golden.cases.length; index += 1) {
    const expected = golden.cases[index];
    const got = actual.cases[index];
    assert.equal(got.name, expected.name);
    assert.equal(got.header, expected.header, `${expected.name}: channel header bytes`);
    assert.equal(got.frame, expected.frame, `${expected.name}: application frame bytes`);
    assert.equal(got.requestHeader, expected.requestHeader, `${expected.name}: request header bytes`);
  }
  assert.equal(actual.replyHeader, golden.replyHeader);
});

test('direct service multipart writes match every stored channel wire case', () => {
  assert.deepEqual(buildCases(dist, true, true), buildCases(dist, false, true));
});

test('multipart application frames equal origin/main across packet-name changes', () => {
  assert.deepEqual(actual.frames, golden.frames);
});

test('golden headers decode to the same fields in any visit order', () => {
  const orders = [
    golden.cases.map((_, index) => index),
    golden.cases.map((_, index) => golden.cases.length - 1 - index),
    golden.cases.map((_, index) => (index * 7) % golden.cases.length),
    golden.cases.flatMap((_, index) => [index, index, 0])
  ];
  for (const order of orders) {
    for (const index of order) {
      const expected = golden.cases[index];
      const decoded = decodeHeader(envelope, Buffer.from(expected.header, 'hex'));
      assert.equal(JSON.stringify(decoded), expected.decoded, expected.name);
    }
  }
});

test('a header that starts with a known prefix but continues differently still decodes fully', () => {
  const base = Buffer.from(golden.cases.find((entry) => entry.name === 'request-deadline-flow').header, 'hex');
  const decodeAgain = () => decodeHeader(envelope, base);
  decodeAgain();
  // A repeated channelName field after the prefix overrides it, as in a full decode.
  const override = Buffer.concat([base, Buffer.from([0x12, 0x03, 0x6f, 0x74, 0x68])]);
  assert.equal(decodeHeader(envelope, override).channelName, 'oth');
  assert.equal(decodeAgain().channelName, 'echo');
  // A truncated header is malformed whether or not its prefix is known.
  assert.throws(() => decodeHeader(envelope, base.subarray(0, base.length - 3)), /malformed/);
  assert.equal(decodeAgain().channelName, 'echo');
});

test('service multipart keeps the generated header in scratch until the final frame write', (t) => {
  const { Writer } = require('protobufjs/minimal');
  const wire = require(`${dist}/runtime/foundation/service-wire-m6a-codec`);
  const encode = (forService) =>
    envelope.encodeChannelEnvelopePartsAtDeadline(
      1, 'echo', 'Echo', { body: '한글' }, 1000, undefined, undefined, 'c', false,
      undefined, forService
    );
  const constants = require(`${dist}/runtime/foundation/service-wire-constants.generated`);
  const expected = wire.encodeMultipartApplicationPayload(
    encode(false), constants.SERVICE_FRAMEWORK_MULTIPART_PACKET_NAME, constants.SERVICE_FRAMEWORK_MULTIPART_CONTENT_TYPE
  );
  const original = Writer.prototype.finish;
  let ownedHeaderBuffers = 0;
  t.mock.method(Writer.prototype, 'finish', function (shared) {
    if (shared !== true) ownedHeaderBuffers++;
    return original.call(this, shared);
  });
  const parts = encode(true);
  // A second encode must be free to reuse framework-owned scratch.
  envelope.encodeChannelEnvelopePartsAtDeadline(
    1, 'echo', 'Echo', { body: 'other' }, 2000, undefined, undefined, 'other', false
  );
  assert.equal(ownedHeaderBuffers, 1);
  const actual = wire.borrowPackedMultipartApplicationFrame(parts);
  assert.equal(ownedHeaderBuffers, 1);
  assert.deepEqual(actual, expected);
});
