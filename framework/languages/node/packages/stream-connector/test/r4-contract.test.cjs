const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const test = require('node:test');
const api = require('../dist');
const { unwrapStreamError } = require('../dist/Runtime/ZlinkStreamSupport');
const fixture = path.resolve(__dirname, '../../../../../test/fixtures/stream-packet-name-whitespace.tsv');

const rows = fs.readFileSync(fixture, 'utf8').split(/\r?\n/).filter((row) => row && !row.startsWith('#'));
assert.ok(rows.length > 0, 'Whitespace fixture must contain cases.');
for (const row of rows) {
  const fields = row.split('\t');
  assert.equal(fields.length, 2);
  const [points, blank] = fields;
  assert.match(points, /^(?:EMPTY|[0-9A-F]{4,6}(?:,[0-9A-F]{4,6})*)$/);
  assert.ok(blank === 'true' || blank === 'false');
  const name = points === 'EMPTY' ? '' : String.fromCodePoint(...points.split(',').map((point) => parseInt(point, 16)));
  test(`R4 Unicode White_Space packet name ${points}`, async () => {
    const connector = api.zlinkStreamConnectorFactory.create({ endpoint: 'ws://node-r4.invalid' });
    if (blank === 'true') {
      assert.throws(() => connector.on(name, () => {}), (error) => error.error?.code === api.ZlinkStreamErrorCode.ValidationFailed);
    } else {
      connector.on(name, () => {}).dispose();
    }
    await connector.close();
  });
}

test('R4 connector error extraction propagates failures without codes', () => {
  for (const failure of [new Error('request timed out'), new TypeError('remote failure'), 'timeout']) {
    assert.throws(() => unwrapStreamError(failure), (actual) => actual === failure);
  }
  const error = { code: api.ZlinkStreamErrorCode.RequestTimeout, message: 'request timed out' };
  assert.strictEqual(unwrapStreamError(new api.ZlinkStreamException(error)), error);
});


test('R4 request failure extraction preserves connector codes in reply hooks and callbacks', async () => {
  const connector = api.zlinkStreamConnectorFactory.create({ endpoint: 'ws://node-r4.invalid', dispatchMode: api.ZlinkStreamDispatchMode.Immediate });
  const replies = [];
  connector.onReplyReceived((reply) => replies.push(reply));
  const payload = { codec: api.ZlinkStreamCodec.Raw, payload: new Uint8Array() };
  await assert.rejects(connector.request(payload).packetName('Request').submit(),
    (failure) => failure.error.code === api.ZlinkStreamErrorCode.Disconnected);
  await connector.dispatch();
  assert.equal(replies.length, 1);
  assert.equal(replies[0].error.code, api.ZlinkStreamErrorCode.Disconnected);
  let completed;
  const callbackCompleted = new Promise((resolve) => { completed = resolve; });
  connector.request(payload).packetName('Request').submit((result) => completed(result));
  const result = await callbackCompleted;
  assert.equal(result.isSuccess, false);
  assert.equal(result.error.code, api.ZlinkStreamErrorCode.Disconnected);
  assert.equal(replies.length, 2);
  await connector.close();
});

test('R4 uncoded custom payload codec failure propagates at public request construction', async () => {
  const failure = new TypeError('custom payload encode failed');
  const connector = api.zlinkStreamConnectorFactory.create({
    endpoint: 'ws://node-r4.invalid',
    codec: { encode() { throw failure; }, decode() { throw failure; } }
  });
  assert.throws(() => connector.request({ value: 1 }), (actual) => actual === failure);
  await connector.close();
});
