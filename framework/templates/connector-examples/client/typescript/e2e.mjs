import assert from 'node:assert/strict';
import { test } from 'node:test';
import {
  zlinkStreamConnectorFactory,
  ZlinkStreamErrorCode,
  ZlinkStreamException
} from '@zlink-systems/stream-connector';
import { runExample } from './dist/example.js';

const endpoint = process.env.CONNECTOR_EXAMPLE_ENDPOINT ?? 'ws://127.0.0.1:7301';

test('connect, receive a push, reconnect, request, leave and close', async () => {
  const lines = [];
  await runExample(endpoint, (line) => lines.push(line));
  assert.deepEqual(lines, [
    'Connected',
    'Push: Hello from the server',
    'Reconnected',
    'Reply: {"score":"9223372036854775807","nickname":null,"label":"default"}',
    'Leave acknowledged',
    'Closed'
  ]);
});

test('the server rejects an int64 number, a missing nickname and a null label', async () => {
  const connector = zlinkStreamConnectorFactory.create({ endpoint, requestTimeoutMs: 5000 });
  try {
    await connector.connect();
    const valid = { score: '42', nickname: null };
    for (const malformed of [{ ...valid, score: 42 }, { score: '42' }, { ...valid, label: null }]) {
      await assert.rejects(connector.request(malformed).packetName('Echo').submit(), (error) => {
        assert.ok(error instanceof ZlinkStreamException);
        assert.equal(error.error.code, ZlinkStreamErrorCode.RemoteError);
        assert.match(error.message, /PayloadDecodeFailed/);
        return true;
      });
    }
    assert.deepEqual(
      await connector
        .request({ ...valid, label: 'custom' })
        .packetName('Echo')
        .submit(),
      { ...valid, label: 'custom' }
    );
  } finally {
    await connector.close();
  }
});
