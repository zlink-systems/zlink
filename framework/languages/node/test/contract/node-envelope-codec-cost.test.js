'use strict';

const assert = require('node:assert/strict');
const test = require('node:test');
const path = require('node:path');
const { spawnSync } = require('node:child_process');

const MODULE_ALLOCATION_FLAG = '--jitless';
const { ZLinkBufferMessage } = require('../../packages/framework/dist/runtime/backend/runtime-message');

test('messaging encode stays within the final Message allocation budget', async t => {
  if (runAllocationTest(t)) return;
  const codec = require('../../packages/framework/dist/runtime/messaging/payload-codec');
  const payload = { body: 'payload' };
  const message = codec.encodeFrameworkPayloadMessage(payload);
  assert.deepEqual(JSON.parse(message.getString()), payload);
  message.close();
  const encode = () => {
    const message = codec.encodeFrameworkPayloadMessage(payload);
    assert.equal(codec.frameworkPayloadContentType(message), 'application/json');
    message.close();
  };
  for (let index = 0; index < 1000; index++) encode();
  const allocations = await trackedCodecAllocations('/runtime/messaging/payload-codec.js', encode);
  // Buffer/JSON work belongs to other modules. An extra per-call descriptor
  // in this module exceeds the near-zero budget.
  assertAllocationBudget(t, allocations, 0.25, 24);
  const encoded = codec.encodeFrameworkPayload({ body: 'explicit descriptor' });
  assert.equal(encoded.contentType, 'application/json');
  encoded.message.close();
});

test('JSON decode stays within the parsed value allocation budget', async t => {
  if (runAllocationTest(t)) return;
  const codec = require('../../packages/framework/dist/runtime/messaging/payload-codec');
  const message = ZLinkBufferMessage.fromOwned(Buffer.from('{"body":"payload"}'));
  const decode = () => codec.decodeFrameworkPayloadMessage(message);
  for (let index = 0; index < 1000; index++) decode();
  const allocations = await trackedCodecAllocations('/runtime/messaging/payload-codec.js', decode);
  // The JSON implementation owns parsing; this module must not allocate a
  // per-call success carrier around the returned value.
  assertAllocationBudget(t, allocations, 0.25, 24);
  assert.deepEqual(decode(), { body: 'payload' });
  assert.throws(
    () => codec.decodeFrameworkPayloadMessage(ZLinkBufferMessage.fromOwned(Buffer.from('{'))),
    error => error.message.includes('PayloadDecodeFailed') && error.cause instanceof SyntaxError
  );
  const configurationFailure = new Error('schema metadata unavailable');
  const declaredType = new Proxy(class Payload {}, {
    getOwnPropertyDescriptor() { throw configurationFailure; }
  });
  assert.throws(
    () => codec.decodeFrameworkPayloadMessage(message, undefined, declaredType),
    error => error === configurationFailure
  );
});

test('channel request and reply stay within the final parts allocation budget', async t => {
  if (runAllocationTest(t)) return;
  const codec = require('../../packages/framework/dist/runtime/channels/channel-envelope');
  const codecs = { serializers: new Map() };
  const data = { body: 'x'.repeat(1024) };
  const roundtrip = () => {
    const parts = codec.encodeChannelEnvelopePartsAtDeadline(
      1, 'codec', 'Packet', data, 2000000000000, undefined, codecs,
      '00112233445566778899aabbccddeeff', false
    );
    const request = codec.decodeChannelHeader(parts.map(part => ZLinkBufferMessage.fromOwned(part)), false);
    const reply = codec.encodeChannelReplyParts(request, data, codecs);
    const response = codec.decodeChannelEnvelope(reply.map(part => ZLinkBufferMessage.fromOwned(part)), undefined, false);
    return codec.decodeChannelPayload(response);
  };
  for (let index = 0; index < 1000; index++) roundtrip();
  const allocations = await trackedCodecAllocations('/runtime/channels/channel-envelope.js', roundtrip);
  // Node 20/22: module-owned final parts and header work stay below 13.5
  // objects / 1100 bytes per roundtrip. Descendant Buffer/JSON allocations
  // are runtime-dependent and are not part of this module's budget.
  assertAllocationBudget(t, allocations, 13.5, 1100);
  assert.deepEqual(roundtrip(), data);
});

test('correlation validation accepts the full ASCII contract within a near-zero allocation budget', async t => {
  if (runAllocationTest(t)) return;
  const codec = require('../../packages/framework/dist/runtime/channels/channel-envelope');
  const valid = ['a'.repeat(64), '\0\x7f'];
  const invalid = ['', 'a'.repeat(65), 'é', '\ud800'];
  for (const value of valid) assert.equal(codec.isValidChannelCorrelationId(value), true);
  for (const value of invalid) assert.equal(codec.isValidChannelCorrelationId(value), false);
  const validate = () => {
    for (const value of valid) codec.isValidChannelCorrelationId(value);
    for (const value of invalid) codec.isValidChannelCorrelationId(value);
  };
  for (let index = 0; index < 1000; index++) validate();
  const allocations = await trackedCodecAllocations('/runtime/channels/channel-envelope.js', validate);
  // Node 20/22: 1000 six-string batches allocate no module-owned objects.
  // The unchanged near-zero budget remains far below one per-call RegExp.
  // Input strings are prepared before tracking.
  assertAllocationBudget(t, allocations, 0.05, 8);
});

test('raw service reply transfers owned bytes directly to the binding reply capability', async () => {
  const { fixture } = require('./fixtures/raw-mesh-close');
  const { ReadyDomain } = require('../../packages/framework/dist/runtime/foundation/service-runtime-contracts');
  const f = fixture();
  let captured;
  let replySource;
  const replyService = f.runtime.replyService.bind(f.runtime);
  f.runtime.replyService = (record, parts) => {
    replySource = record;
    replyService(record, parts);
  };
  const payload = Buffer.alloc(1024, 7);
  try {
    const input = {
      owner: 'local', domain: 'application', sourceRoutingId: 'source',
      requestSequence: 1n, parts: [Buffer.from('service request')],
      reply(parts) { captured = parts; }
    };
    f.runtime.mailbox.tryEnqueue(input);
    const ready = f.backend.createReadyBatch(1);
    f.backend.drainReady(ReadyDomain.Application, ready);
    const claim = ready.takeClaim(0);
    const result = claim.recvBatch(f.backend.createReceiveBatch(1, 1));
    try {
      result.records[0].reply([ZLinkBufferMessage.fromOwned(payload)]);
      assert.equal(captured[0], payload, 'Framework must not snapshot before the synchronous binding reply.');
      assert.equal(captured[0].length, 1024);
      assert.equal(replySource, input, 'Reply routing uses the owning record without a snapshot.');
    } finally {
      for (const record of result.records) {
        for (const part of record.parts) part.close();
        record.releaseRetainedIngress?.();
      }
      claim.release();
      ready.close();
    }
  } finally {
    await f.backend.close();
  }
});

test('reply decode borrows its body once for JSON, registered codecs and raw bytes', () => {
  const codec = require('../../packages/framework/dist/runtime/channels/channel-envelope');
  for (const contentType of ['application/json', 'application/x-test', 'application/octet-stream']) {
    const body = Buffer.from(contentType === 'application/json' ? '{"body":"reply"}' : 'reply');
    const header = Buffer.from(JSON.stringify({
      formatMarker: 242, kind: 2, channelName: 'codec', messageName: 'Packet',
      contentType, correlationId: 'request', deadline: null, topic: null, metadata: {}
    }));
    let reads = 0;
    const serializer = { deserialize: payload => payload.getString() };
    const result = codec.decodeChannelReply([
      ZLinkBufferMessage.fromOwned(header),
      { data() { reads++; return body; } }
    ], { serializers: new Map([['application/x-test', serializer]]) });
    assert.equal(reads, 1);
    if (contentType === 'application/json') assert.deepEqual(result, { body: 'reply' });
    else if (contentType === 'application/octet-stream') assert.equal(result, body);
    else assert.equal(result, 'reply');
  }
});

// JIT compilation both allocates bookkeeping objects and moves allocation
// attribution across module boundaries through inlining. Measure in an isolated
// interpreter process so module-owned allocations have the same meaning on each
// supported runtime. Product source and the normal runtime are unchanged.
function runAllocationTest(t) {
  if (process.execArgv.includes(MODULE_ALLOCATION_FLAG)) return false;
  const result = spawnSync(process.execPath, [
    MODULE_ALLOCATION_FLAG, '--test', '--test-reporter=tap',
    `--test-name-pattern=^${t.name}$`, __filename
  ], { encoding: 'utf8', env: { ...process.env, NODE_TEST_CONTEXT: undefined } });
  assert.ifError(result.error);
  assert.equal(result.status, 0, `${result.stdout}\n${result.stderr}`);
  // A nested runner can exit successfully without executing the selected test.
  assert.match(result.stdout, /^# pass 1$/m, result.stdout);
  assert.match(result.stdout, /^# fail 0$/m, result.stdout);
  for (const line of result.stdout.split('\n')) {
    if (line.startsWith('# 1000 calls:')) t.diagnostic(line.slice(2));
  }
  return true;
}

// Track the real built module, without rewriting source. trace_tree records
// allocations even when the objects are collected before the final snapshot.
// Sum frames in this module only, excluding descendant Buffer/JSON internals.
async function trackedCodecAllocations(filename, action) {
  const inspector = require('node:inspector');
  const session = new inspector.Session();
  session.connect();
  const post = (method, params = {}) => new Promise((resolve, reject) => {
    session.post(method, params, (error, result) => error ? reject(error) : resolve(result));
  });
  const chunks = [];
  try {
    await post('HeapProfiler.enable');
    await post('HeapProfiler.startTrackingHeapObjects', { trackAllocations: true });
    for (let index = 0; index < 1000; index++) action();
    session.on('HeapProfiler.addHeapSnapshotChunk', ({ params }) => chunks.push(params.chunk));
    await post('HeapProfiler.stopTrackingHeapObjects', { reportProgress: false });
  } finally {
    session.disconnect();
  }
  const snapshot = JSON.parse(chunks.join(''));
  const fields = snapshot.snapshot.meta.trace_function_info_fields;
  const infos = snapshot.trace_function_infos;
  const allocations = new Map();
  function visit(nodes) {
    for (let index = 0; index < nodes.length; index += 5) {
      const offset = nodes[index + 1] * fields.length;
      const name = snapshot.strings[infos[offset + fields.indexOf('name')]];
      const script = snapshot.strings[infos[offset + fields.indexOf('script_name')]];
      if (script.endsWith(filename)) {
        const entry = allocations.get(name) ?? { objects: 0, bytes: 0 };
        entry.objects += nodes[index + 2];
        entry.bytes += nodes[index + 3];
        allocations.set(name, entry);
      }
      visit(nodes[index + 4]);
    }
  }
  visit(snapshot.trace_tree);
  return allocations;
}

function assertAllocationBudget(t, allocations, objectsPerCall, bytesPerCall) {
  const total = [...allocations.values()].reduce((sum, entry) => ({
    objects: sum.objects + entry.objects,
    bytes: sum.bytes + entry.bytes
  }), { objects: 0, bytes: 0 });
  t.diagnostic(`1000 calls: ${total.objects} objects, ${total.bytes} bytes`);
  assert.ok(total.objects <= 1000 * objectsPerCall,
    `${total.objects / 1000} objects/call exceeds ${objectsPerCall}`);
  assert.ok(total.bytes <= 1000 * bytesPerCall,
    `${total.bytes / 1000} bytes/call exceeds ${bytesPerCall}`);
}

test('warmed serializer lookup does not allocate preparation closure contexts', async t => {
  if (runAllocationTest(t)) return;
  const codec = require('../../packages/framework/dist/runtime/messaging/payload-codec');
  class Payload {}
  const payload = new Payload();
  const serializers = new Map([['application/x-test', {}]]);
  const lookup = () => codec.selectSerializerWithContentType(payload, serializers);
  for (let index = 0; index < 1000; index++) lookup();
  const allocations = await trackedCodecAllocations('/runtime/messaging/payload-codec.js', lookup);
  assert.equal(allocations.get('serializerSelectionPlanOf')?.objects ?? 0, 0);
});

// A canonical header has a fixed final shape. Adding fields after parsing
// otherwise grows V8 property storage twice on every received header.
test('canonical header validation does not allocate property growth storage', async t => {
  if (runAllocationTest(t)) return;
  const codec = require('../../packages/framework/dist/runtime/channels/channel-envelope');
  const bytes = Buffer.from('{"formatMarker":242,"kind":1,"channelName":"codec","messageName":"Packet","contentType":"application/json","correlationId":"request","deadline":null,"topic":null,"metadata":{}}');
  const parts = [ZLinkBufferMessage.fromOwned(bytes)];
  const decode = () => codec.decodeChannelHeader(parts, false);
  for (let index = 0; index < 1000; index++) decode();
  const allocations = await trackedCodecAllocations('/runtime/channels/channel-envelope.js', decode);
  // The reader and final header account for two module-owned objects.
  // Property growth must not add storage in any helper in this module.
  assertAllocationBudget(t, allocations, 2.25, 240);
  assert.equal(allocations.get('validateChannelHeader')?.objects ?? 0, 0);
  assert.deepEqual(decode().metadata, {});
});

test('correlation IDs use the fixed zero prefix without constructing padStart intermediates', t => {
  const codec = require('../../packages/framework/dist/runtime/channels/channel-envelope');
  const padStart = t.mock.method(String.prototype, 'padStart');
  const ids = new Set();
  for (let index = 0; index < 1000; index++) {
    const id = codec.newChannelCorrelationId();
    assert.match(id, /^[0-9a-f]{32}$/);
    ids.add(id);
  }
  assert.equal(ids.size, 1000);
  assert.equal(padStart.mock.callCount(), 0);
});

test('creation payload decode borrows the encoded body without intermediate byte copies', t => {
  const codec = require('../../packages/framework/dist/runtime/messaging/creation-payload-codec');
  const wire = require('../../packages/framework/dist/runtime/foundation/service-wire-m6a-codec');
  const value = { body: 'x'.repeat(1024) };
  const frame = codec.encodeFrameworkCreationPayload(value);
  const bodyBytes = wire.decodeApplicationPayloadView(frame).payload.byteLength;
  const from = Buffer.from;
  let copiedBytes = 0;
  t.mock.method(Buffer, 'from', function (input, ...args) {
    if (ArrayBuffer.isView(input)) copiedBytes += input.byteLength;
    return from.call(Buffer, input, ...args);
  });
  for (let index = 0; index < 1000; index++) {
    assert.deepEqual(codec.decodeFrameworkCreationPayload(frame), value);
  }
  assert.equal(copiedBytes, 0, `Avoid copying the ${bodyBytes}-byte encoded body twice per decode.`);
});


test('JSON parser policies are immutable and reused without per-decode option objects', t => {
  const profilePath = require.resolve('@zlink-systems/stream-wire/internal/json');
  const implementation = require(path.join(path.dirname(profilePath), 'framework-json-v1.js'));
  const parse = implementation.parseFrameworkJsonV1;
  const policies = [];
  t.mock.method(implementation, 'parseFrameworkJsonV1', (text, options = {}, ...args) => {
    policies.push(options);
    return parse(text, options, ...args);
  });
  // main exports the profile function directly, so load after installing the
  // spy rather than observing a previously cached function reference.
  const jsonPath = require.resolve('../../packages/framework/dist/runtime/messaging/framework-json-v1');
  const cachedJson = require.cache[jsonPath];
  delete require.cache[jsonPath];
  t.after(() => { require.cache[jsonPath] = cachedJson; });
  const json = require(jsonPath);
  assert.deepEqual(json.parseFrameworkJsonV1('{"body":1}'), { body: 1 });
  assert.deepEqual(json.parseFrameworkJsonV1('{"body":2}'), { body: 2 });
  assert.equal(policies.length, 2);
  assert.equal(policies[0], policies[1]);
  assert.equal(typeof policies[0], 'object');
  assert.equal(Object.isFrozen(policies[0]), true);
  const explicit = { rejectPropertyName: name => name === 'blocked' };
  assert.throws(() => json.parseFrameworkJsonV1('{"blocked":1}', explicit));
  assert.equal(policies.length, 3);
  assert.equal(policies[2], explicit);

  const frameworkJson = json;
  const parseFramework = frameworkJson.parseFrameworkJsonV1;
  const channelPolicies = [];
  t.mock.method(frameworkJson, 'parseFrameworkJsonV1', (text, options, ...args) => {
    channelPolicies.push(options);
    return parseFramework(text, options, ...args);
  });
  const channelPath = require.resolve('../../packages/framework/dist/runtime/channels/channel-envelope');
  const cachedChannel = require.cache[channelPath];
  delete require.cache[channelPath];
  t.after(() => { require.cache[channelPath] = cachedChannel; });
  const channel = require(channelPath);
  const header = { contentType: 'application/json', kind: 1, messageName: 'Packet' };
  for (let index = 0; index < 2; index++) {
    assert.deepEqual(channel.decodeChannelPayload({ header, payload: Buffer.from('{"body":1}') }), { body: 1 });
  }
  assert.equal(channelPolicies.length, 2);
  assert.equal(channelPolicies[0], channelPolicies[1]);
  assert.equal(typeof channelPolicies[0], 'object');
  assert.equal(Object.isFrozen(channelPolicies[0]), true);
  assert.throws(() => channel.decodeChannelPayload({ header, payload: Buffer.from('{"__proto__":1}') }));
});

// Compare against V8's own stringify allocations in the same process so the
// budget follows the runtime's object layout rather than fixed heap bytes.
function nativeJsonEncoding(value) {
  return JSON.stringify(value, identityJsonValue);
}
function identityJsonValue(_key, value) { return value; }
function allocationTotal(allocations) {
  return [...allocations.values()].reduce((sum, entry) => sum + entry.objects, 0);
}

test('default JSON encoding adds no unused custom-replacer context', async t => {
  if (runAllocationTest(t)) return;
  const json = require('../../packages/stream-wire/dist/internal/framework-json-v1');
  const value = { body: 'x'.repeat(1024) };
  const encode = () => json.stringifyFrameworkJsonV1(value);
  const control = () => nativeJsonEncoding(value);
  for (let index = 0; index < 1000; index++) { encode(); control(); }
  const baseline = await trackedCodecAllocations('/node-envelope-codec-cost.test.js', control);
  const allocations = await trackedCodecAllocations('/internal/framework-json-v1.js', encode);
  const expected = baseline.get('nativeJsonEncoding').objects;
  const actual = allocationTotal(allocations);
  t.diagnostic(`1000 calls: native=${expected}, strict=${actual} objects`);
  assert.equal(encode(), control());
  assert.ok(actual <= expected, `${actual} strict allocations exceed ${expected} native allocations`);
});

test('custom JSON validation adds no carrier per property', async t => {
  if (runAllocationTest(t)) return;
  const json = require('../../packages/stream-wire/dist/internal/framework-json-v1');
  const small = { body: 1 };
  const large = Object.fromEntries(Array.from({ length: 32 }, (_, index) => [`p${index}`, index]));
  const encode = value => json.stringifyFrameworkJsonV1(value, undefined, identityJsonValue);
  for (let index = 0; index < 1000; index++) { encode(small); encode(large); }
  const smallAllocations = await trackedCodecAllocations('/internal/framework-json-v1.js', () => encode(small));
  const largeAllocations = await trackedCodecAllocations('/internal/framework-json-v1.js', () => encode(large));
  const smaller = allocationTotal(smallAllocations);
  const larger = allocationTotal(largeAllocations);
  t.diagnostic(`1000 calls: one-property=${smaller}, 32-properties=${larger} objects`);
  assert.equal(encode(large), JSON.stringify(large));
  assert.ok(larger <= smaller, `${larger} allocations grow with properties from ${smaller}`);
});

test('custom JSON replacement preserves holder, key order and strict value validation', () => {
  const json = require('../../packages/stream-wire/dist/internal/framework-json-v1');
  const source = { finite: NaN, integer: 7n, bytes: Uint8Array.from([0, 255]) };
  const keys = [];
  const encoded = json.stringifyFrameworkJsonV1(source, undefined, function (key, value) {
    keys.push(key);
    if (key !== '') assert.equal(this, source);
    return key === 'finite' ? 3 : value;
  });
  assert.deepEqual(keys, ['', 'finite', 'integer', 'bytes']);
  assert.equal(encoded, '{"finite":3,"integer":"7","bytes":"AP8="}');
  for (const invalid of [Infinity, new Date(0), { toJSON() { return 1; } }]) {
    assert.throws(() => json.stringifyFrameworkJsonV1(0, undefined, () => invalid), TypeError);
  }
  assert.throws(() => json.stringifyFrameworkJsonV1(0, undefined, () => 1n << 64n), RangeError);
  const failure = new Error('custom replacer failed');
  assert.throws(() => json.stringifyFrameworkJsonV1(0, undefined, () => { throw failure; }),
    error => error === failure);
});
