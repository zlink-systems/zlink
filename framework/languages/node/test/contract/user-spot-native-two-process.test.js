const assert = require('node:assert/strict');
const path = require('node:path');
const test = require('node:test');
const { createClient } = require('redis');

const fixture = path.join(__dirname, 'fixtures', 'user-spot-native-process.js');
const redisUrl = process.env.ZLINK_TEST_REDIS_URL ?? 'redis://127.0.0.1:6379';
const { startChild, stopChild, deletePrefix } = require('./helpers/native-process-harness')(
  fixture,
  redisUrl
);

test(
  'public SpotManager completes User Spot lifecycle through two native MeshNode processes',
  { timeout: 60_000 },
  async (t) => {
    const runId = `${process.pid}-${Date.now()}-${Math.random().toString(16).slice(2)}`;
    const meshName = `m6b-native-${runId}`;
    const keyPrefix = `zlink:test:m6b-node:${runId}:`;
    const targetRoutingId = `m6b-target-${runId}`;
    const children = [];
    const redis = createClient({
      url: redisUrl,
      socket: { reconnectStrategy: false }
    });
    redis.on('error', () => {});
    try {
      await redis.connect();
      await redis.ping();
    } catch (error) {
      if (redis.isOpen) redis.disconnect();
      t.skip(`Redis is unavailable at ${redisUrl}: ${error.message}`);
      return;
    }
    t.after(async () => {
      await Promise.all(children.map(stopChild));
      await deletePrefix(redis, keyPrefix);
      if (redis.isOpen) await redis.quit();
    });

    const target = startChild(children, {
      role: 'target',
      meshName,
      // Each node binds an OS-assigned port and publishes the endpoint it bound.
      endpoint: 'tcp://127.0.0.1:*',
      routingId: targetRoutingId,
      targetRoutingId,
      keyPrefix,
      pauseFirstClose: true
    });
    const targetReady = await target.ready;
    assert.equal(targetReady.role, 'target');

    const source = startChild(children, {
      role: 'source',
      meshName,
      endpoint: 'tcp://127.0.0.1:*',
      routingId: `m6b-source-${runId}`,
      targetRoutingId,
      keyPrefix,
      targetEndpoint: targetReady.endpoint
    });
    const sourceReady = await source.ready;
    assert.equal(sourceReady.role, 'source');
    assert.notEqual(sourceReady.pid, targetReady.pid);
    assert.equal(await source.command('waitRouteReady'), true);
    const created = await source.command('create');
    assert.equal(created.state, 'created');
    assert.equal(created.spot.meshName, meshName);
    assert.equal(created.spot.nodeRid, targetRoutingId);

    const existing = await source.command('getOrCreate');
    assert.equal(existing.state, 'existing');
    assert.deepEqual(existing.spot, created.spot);

    assert.deepEqual(await source.command('find'), created.spot);
    const close = source.command('close');
    await target.waitForEvent('close-entered');
    await target.command('releaseFirstClose');
    assert.equal(await close, true);
    assert.equal(await target.command('closeExecutions'), 1);
    assert.equal(await source.command('findAfterClose'), null);
  }
);
