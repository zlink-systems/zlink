'use strict';
const assert = require('node:assert/strict');
const path = require('node:path');
const test = require('node:test');
const { createClient } = require('redis');
const {
  ServiceStatefulRuntime
} = require('../../packages/framework/dist/runtime/foundation/service-stateful-runtime');
const {
  ServiceStaleGenerationError
} = require('../../packages/framework/dist/runtime/foundation/service-stateful-registry');
const redisUrl = process.env.ZLINK_TEST_REDIS_URL ?? 'redis://127.0.0.1:6379';
const { startChild, stopChild, deletePrefix } = require('./helpers/native-process-harness')(
  path.join(__dirname, 'fixtures', 'actor-direct-route-process.js'),
  redisUrl
);

test(
  'cached ID request reaches the recreated Actor on the same owner',
  { timeout: 60000 },
  async (t) => {
    const run = `${process.pid}-${Date.now()}`;
    const children = [];
    const keyPrefix = `zlink:test:recreated:${run}:`;
    const redis = createClient({ url: redisUrl, socket: { reconnectStrategy: false } });
    redis.on('error', () => {});
    await redis.connect();
    t.after(async () => {
      await Promise.all(children.map(stopChild));
      await deletePrefix(redis, keyPrefix);
      await redis.quit();
    });
    const base = {
      meshName: `recreated-${run}`,
      keyPrefix,
      endpoint: 'tcp://127.0.0.1:*',
      targetRoutingId: `owner-${run}`
    };
    const owner = startChild(children, {
      ...base,
      role: 'target',
      routingId: base.targetRoutingId
    });
    const ready = await owner.ready;
    await owner.command('createActor');
    const caller = startChild(children, {
      ...base,
      role: 'client',
      routingId: `client-${run}`,
      targetEndpoint: ready.endpoint
    });
    await caller.ready;
    await caller.command('waitRouteReady');
    const first = await caller.command('askGeneration');
    assert.equal(await caller.command('actorRouteCached'), true);
    assert.equal(await owner.command('destroyActor'), true);
    await owner.command('createActor');
    assert.equal(await caller.command('actorRouteCached'), true);
    const second = await caller.command('askGeneration');
    assert.equal(second.node, first.node);
    assert.notEqual(second.generation, first.generation);
  }
);

test('direct Actor admission rejects an old owner of the same incarnation', () => {
  const runtime = new ServiceStatefulRuntime(
    {
      observePeerRoute: () => ({ generation: 1n, ready: true }),
      observePeerRouteChanged: () => () => {},
      observePeerConnectionIntentRemoved: () => () => {},
      setServiceIngress() {}
    },
    'node-a',
    3n
  );
  try {
    const previous = runtime.createActor('actor-a');
    runtime.discardRelocatedActor(previous.ref);
    const current = runtime.createActor('actor-a');
    const cached = {
      actor: previous.ref,
      targetNodeGeneration: 3n,
      authorityOwnerGeneration: previous.authorityOwnerGeneration,
      ownerLeaseGeneration: 3n
    };
    assert.equal(runtime.validateActorMessageFence(cached), current);
    assert.throws(
      () => runtime.validateActorMessageFence({ ...cached, actor: current.ref }),
      ServiceStaleGenerationError
    );
  } finally {
    runtime.close();
  }
});
