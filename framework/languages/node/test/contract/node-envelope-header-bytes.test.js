const test = require('node:test');
const assert = require('node:assert/strict');
const {
  encodeChannelEnvelopePartsAtDeadline,
  encodeChannelReplyParts,
  decodeChannelHeader
} = require('../../packages/framework/dist/runtime/channels/channel-envelope');
const { runWithFlow } = require('../../packages/framework/dist/runtime/diagnostics/flow-context');

const id = '00112233445566778899aabbccddeeff';
const flow = { flowId: '01920000-0000-7abc-8def-0123456789ab', flowOrigin: 'Application' };
const payload = Buffer.from('payload');
const decode = (bytes) => decodeChannelHeader([{ data: () => bytes }], true);

function expectedHeader(
  kind,
  channelName,
  messageName,
  correlationId,
  deadline,
  metadata,
  withFlow
) {
  const header = {
    formatMarker: 242,
    kind,
    channelName,
    messageName,
    contentType: 'application/octet-stream',
    correlationId,
    deadline,
    topic: null
  };
  if (kind !== 2) {
    header.errorCode = null;
    header.errorMessage = null;
  }
  header.metadata = metadata;
  if (withFlow) {
    header.flowId = flow.flowId;
    header.flowOrigin = 3;
  }
  return Buffer.from(JSON.stringify(header));
}

function assertHeader(actual, expected) {
  assert.deepEqual(actual, expected);
  const value = JSON.parse(expected);
  assert.deepEqual(decode(actual), {
    ...value,
    errorCode: value.errorCode ?? null,
    errorMessage: value.errorMessage ?? null,
    source: undefined,
    flowId: value.flowId,
    flowOrigin: value.flowOrigin === undefined ? undefined : 'Application'
  });
  const changed = Buffer.from(expected);
  changed[changed.indexOf('242')] = 0x31;
  assert.throws(() => assert.deepEqual(actual, changed), assert.AssertionError);
  assert.throws(() => decode(changed));
}

test('request, reply and command headers preserve literal field order and escaped bytes', () => {
  const deadline = 2000000000000;
  for (const codecs of [undefined, { serializers: new Map() }]) {
    for (const names of [
      ['bench', 'Packet'],
      ['quote"slash\\line\n한글😀', 'control\t\u0001']
    ]) {
      for (const correlation of [id, 'x'.repeat(64), '\0"\\\n'.repeat(16)]) {
        for (const withFlow of [false, true]) {
          for (const metadata of [new Map(), new Map([['tenant"\\', 'north\n한글😀']])]) {
            for (const timestamp of [undefined, deadline]) {
              for (const kind of [1, 3]) {
                const correlationId = kind === 1 ? correlation : undefined;
                const encoded = runWithFlow(withFlow ? flow : undefined, () =>
                  encodeChannelEnvelopePartsAtDeadline(
                    kind,
                    names[0],
                    names[1],
                    payload,
                    timestamp,
                    undefined,
                    codecs,
                    correlationId,
                    withFlow,
                    metadata
                  )
                );
                const expected = expectedHeader(
                  kind,
                  ...names,
                  correlationId ?? null,
                  timestamp === undefined ? null : '2033-05-18T03:33:20.000Z',
                  Object.fromEntries(metadata),
                  withFlow
                );
                assertHeader(encoded[0], expected);
                assert.strictEqual(encoded[1], payload);
                if (kind === 1) {
                  const request = decode(encoded[0]);
                  const reply = encodeChannelReplyParts(request, payload, codecs);
                  assertHeader(
                    reply[0],
                    expectedHeader(2, ...names, correlation, null, {}, withFlow)
                  );
                  assert.strictEqual(reply[1], payload);
                }
              }
            }
          }
        }
      }
    }
  }
});

test('deadline formatting preserves ISO bytes across calendar and expanded-year boundaries', () => {
  const dates = [
    1, 999, 1000, 86399999, 86400000, 951782399999, 951782400000, 4107542399999, 4107542400000,
    253402300799999, 253402300800000, 8640000000000000
  ];
  // Deterministic coverage across Date's full positive domain, independent of the encoder.
  let seed = 123456789;
  for (let index = 0; index < 2000; index++) {
    seed = (Math.imul(seed, 1664525) + 1013904223) >>> 0;
    dates.push(Math.floor((seed / 2 ** 32) * 8640000000000000) + 1);
  }
  for (const codecs of [undefined, { serializers: new Map() }]) {
    for (const deadline of dates) {
      const parts = encodeChannelEnvelopePartsAtDeadline(
        1,
        'bench',
        'Packet',
        payload,
        deadline,
        undefined,
        codecs,
        id,
        false
      );
      assertHeader(
        parts[0],
        expectedHeader(1, 'bench', 'Packet', id, new Date(deadline).toISOString(), {}, false)
      );
    }
    for (const deadline of [0, -1, NaN, Infinity, 1.5, 8640000000000001, Number.MAX_SAFE_INTEGER]) {
      assert.throws(
        () =>
          encodeChannelEnvelopePartsAtDeadline(
            1,
            'bench',
            'Packet',
            payload,
            deadline,
            undefined,
            codecs,
            id,
            false
          ),
        RangeError
      );
    }
  }
});

test('request correlation rejects invalid inputs before encoding while accepting every ASCII byte', () => {
  const codecs = { serializers: new Map() };
  for (let byte = 0; byte < 128; byte++) {
    const correlation = String.fromCharCode(byte).repeat(64);
    const parts = encodeChannelEnvelopePartsAtDeadline(
      1,
      'bench',
      'Packet',
      payload,
      2000000000000,
      undefined,
      codecs,
      correlation,
      false
    );
    assertHeader(
      parts[0],
      expectedHeader(1, 'bench', 'Packet', correlation, '2033-05-18T03:33:20.000Z', {}, false)
    );
  }
  for (const correlation of ['', 'x'.repeat(65), 'é', '\ud800', '😀']) {
    assert.throws(() =>
      encodeChannelEnvelopePartsAtDeadline(
        1,
        'bench',
        'Packet',
        payload,
        2000000000000,
        undefined,
        codecs,
        correlation,
        false
      )
    );
  }
});
