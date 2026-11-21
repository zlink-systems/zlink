const assert = require('node:assert/strict');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');
const test = require('node:test');
const {
  ZLinkFrameworkRelocationOutcome,
  ZLinkFrameworkErrorKind
} = require('../../packages/framework/dist');
const fixture = path.join(__dirname, 'fixtures', 'actor-direct-route-process.js');
const {
  ZLinkFrameworkInternalErrorKind
} = require('../../packages/framework/dist/runtime/framework-errors-internal');
const { connectTestRedis } = require('./helpers/test-redis');
const { startChild, stopChild, deletePrefix } = require('./helpers/native-process-harness')(
  fixture
);

for (const scenario of [
  'cold',
  'warm-send',
  'warm-request',
  'resolved-send',
  'resolved-request',
  'owner-change'
]) {
  test(
    `Actor direct uses current membership after User Spot Close (${scenario} remote route cache)`,
    { timeout: 60_000 },
    async (t) => {
      const runId = `${process.pid}-${Date.now()}-${scenario}`;
      const meshName = `actor-direct-${runId}`;
      const keyPrefix = `zlink:test:actor-direct:${runId}:`;
      const spotId = `user-spot-${runId}`;
      const targetRoutingId = `owner-${runId}`;
      const nextRoutingId = `next-${runId}`;
      const flowDirectory =
        process.env.ZLINK_NODE_BOOTSTRAP_FLOW_DIR ??
        fs.mkdtempSync(path.join(os.tmpdir(), 'actor-direct-'));
      const children = [];
      const redis = await connectTestRedis(t);
      if (redis === undefined) {
        if (!process.env.ZLINK_NODE_BOOTSTRAP_FLOW_DIR)
          fs.rmSync(flowDirectory, { recursive: true, force: true });
        return;
      }
      t.after(async () => {
        await Promise.all(children.map(stopChild));
        await deletePrefix(redis, keyPrefix);
        if (redis.isOpen) await redis.quit();
        if (!process.env.ZLINK_NODE_BOOTSTRAP_FLOW_DIR)
          fs.rmSync(flowDirectory, { recursive: true, force: true });
      });
      const base = {
        signal: t.signal,
        meshName,
        keyPrefix,
        targetRoutingId,
        nextRoutingId,
        spotId,
        flowDirectory,
        endpoint: 'tcp://127.0.0.1:*'
      };
      const target = startChild(children, { ...base, role: 'target', routingId: targetRoutingId });
      const ready = await target.ready;
      const source = startChild(children, {
        ...base,
        role: 'source',
        routingId: `sender-${runId}`,
        targetEndpoint: ready.endpoint
      });
      assert.notEqual((await source.ready).pid, ready.pid);
      await source.command('waitRouteReady');
      assert.equal((await target.command('create')).state, 'created');
      await target.command('createActor');
      await target.command('join');
      assert.equal((await target.waitForEvent('joined')).status, 'accepted');
      if (scenario.startsWith('warm')) assert.equal((await source.command('ask')).spot, spotId);
      if (scenario.startsWith('resolved'))
        await source.command(scenario.endsWith('send') ? 'prepareNotice' : 'prepareAsk');
      const cached = await source.command('actorRouteCached');
      assert.equal(cached, scenario.startsWith('warm') || scenario.startsWith('resolved'));
      await target.command('entry');
      assert.equal((await target.waitForEvent('joined')).status, 'accepted');
      assert.equal(await target.command('close'), true);
      assert.equal(
        await source.command('actorRouteCached'),
        cached,
        'remote Join and Close must not disable the sender route cache'
      );
      await target.command('notice');
      assert.notEqual((await target.waitForEvent('notice')).spot, spotId);
      if (scenario.startsWith('resolved')) {
        const result = await source.command('resumeDelivery');
        if (scenario.endsWith('request')) assert.notEqual(result.spot, spotId);
        else assert.notEqual((await target.waitForEvent('notice')).spot, spotId);
      }
      if (scenario === 'warm-send') {
        await source.command('notice');
        assert.notEqual((await target.waitForEvent('notice')).spot, spotId);
      }
      assert.notEqual((await source.command('ask')).spot, spotId);
      await source.command('notice');
      assert.notEqual((await target.waitForEvent('notice')).spot, spotId);
      if (scenario === 'warm-request') {
        const version = await source.command('actorRouteCached');
        await assert.rejects(source.command('applicationNotFound'), (error) => {
          const failure = JSON.parse(error.message);
          return (
            failure.kind === ZLinkFrameworkErrorKind.NotFound &&
            failure.internalKind === undefined &&
            failure.message === 'application result'
          );
        });
        assert.equal(await source.command('actorRouteCached'), version);
        assert.equal(await target.command('destroyActor'), true);
        await assert.rejects(
          source.command('ask'),
          (error) =>
            JSON.parse(error.message).internalKind ===
            ZLinkFrameworkInternalErrorKind.RequestTargetNotFound
        );
        assert.equal(await source.command('actorRouteCached'), false);
        await assert.rejects(
          source.command('ask'),
          (error) =>
            JSON.parse(error.message).internalKind ===
            ZLinkFrameworkInternalErrorKind.ActorRouteNotFound
        );
      }
      if (scenario === 'owner-change') {
        assert.equal(await source.command('actorRouteCached'), true);
        const previous = await target.command('findActor');
        const next = startChild(children, {
          ...base,
          role: 'target',
          routingId: nextRoutingId,
          targetEndpoint: ready.endpoint
        });
        await next.ready;
        await next.command('waitRouteReady');
        await source.command('waitNewOwner');
        // Fix this operation's route before relocation without relying on cache lifetime.
        await source.command('prepareAsk');
        const terminal = await target.command('relocate');
        assert.equal(terminal.outcome, ZLinkFrameworkRelocationOutcome.Relocated);
        const current = await next.command('findActor');
        assert.equal(current.nodeRid, nextRoutingId);
        assert.notEqual(current.nodeRid, previous.nodeRid);
        assert.equal(current.objectGeneration, previous.objectGeneration);
        // The request fixed to the old owner arrives after relocation commit. The
        // old owner relays it over the committed Message Follow route to the new
        // owner without resubmission (01-location-runtime §7.3).
        assert.equal((await source.command('resumeDelivery')).node, nextRoutingId);
        // A valid messageFollow notice invalidates the sender cache so the next
        // lookup reaches the new owner directly (08-routing:505). The notice is a
        // one-way command with no order against the relayed reply, so observe its
        // arrival rather than the cache state at the reply.
        const invalidateDeadline = Date.now() + 5_000;
        while (
          (await source.command('actorRouteCached')) !== false &&
          Date.now() < invalidateDeadline
        )
          await new Promise((resolve) => setTimeout(resolve, 20));
        assert.equal(await source.command('actorRouteCached'), false);
        assert.equal((await source.command('ask')).node, nextRoutingId);
      }
    }
  );
}
