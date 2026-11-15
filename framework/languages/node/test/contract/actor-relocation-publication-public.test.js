'use strict';
const assert = require('node:assert/strict');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');
const test = require('node:test');
const { createClient } = require('redis');
const {
  ZLinkFrameworkErrorKind,
  ZLinkFrameworkRelocationOutcome
} = require('../../packages/framework/dist');
const redisUrl = process.env.ZLINK_TEST_REDIS_URL ?? 'redis://127.0.0.1:6379';
const { startChild, stopChild, deletePrefix } = require('./helpers/native-process-harness')(
  path.join(__dirname, 'fixtures', 'actor-direct-route-process.js'),
  redisUrl
);

for (const boundary of ['Find', 'Entry request']) {
  test(
    `public ${boundary} uses its authority boundary during relocation staging`,
    { timeout: 60_000 },
    async (t) => {
      const runId = `${process.pid}-${Date.now()}`;
      const ownerRid = `owner-${runId}`;
      const nextRid = `next-${runId}`;
      const keyPrefix = `zlink:test:publication:${runId}:`;
      const flowDirectory =
        process.env.ZLINK_NODE_BOOTSTRAP_FLOW_DIR ??
        fs.mkdtempSync(path.join(os.tmpdir(), 'actor-publication-'));
      const children = [];
      const redis = createClient({ url: redisUrl, socket: { reconnectStrategy: false } });
      redis.on('error', () => {});
      await redis.connect();
      let next;
      let relocation;
      t.after(async () => {
        if (next !== undefined) await next.command('resumePublication');
        if (relocation !== undefined) await relocation;
        await Promise.all(children.map(stopChild));
        await deletePrefix(redis, keyPrefix);
        await redis.quit();
        if (!process.env.ZLINK_NODE_BOOTSTRAP_FLOW_DIR)
          fs.rmSync(flowDirectory, { recursive: true, force: true });
      });
      const base = {
        role: 'target',
        meshName: `publication-${runId}`,
        keyPrefix,
        targetRoutingId: ownerRid,
        nextRoutingId: nextRid,
        flowDirectory,
        endpoint: 'tcp://127.0.0.1:*'
      };
      const owner = startChild(children, { ...base, routingId: ownerRid });
      const ready = await owner.ready;
      await owner.command('createActor');
      assert.equal((await owner.command('ask')).node, ownerRid);
      const target = startChild(children, {
        ...base,
        routingId: nextRid,
        targetEndpoint: ready.endpoint
      });
      await target.ready;
      await target.command('waitRouteReady');
      await owner.command('waitNewOwner');
      await target.command('holdPublication');
      next = target;
      relocation = owner.command('relocate');
      await target.waitForEvent('publication-held');
      assert.equal((await relocation).outcome, ZLinkFrameworkRelocationOutcome.Relocated);
      if (boundary === 'Find') {
        const current = await target.command('findActor');
        assert.equal(
          current?.nodeRid,
          nextRid,
          'local staging must not hide the current Ready Store authority'
        );
        assert.equal(current.objectGeneration, '1');
      } else {
        await assert.rejects(
          target.command('ask'),
          (error) => JSON.parse(error.message).kind === ZLinkFrameworkErrorKind.NotFound
        );
      }
    }
  );
}
