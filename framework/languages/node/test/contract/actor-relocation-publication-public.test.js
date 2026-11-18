'use strict';
const assert = require('node:assert/strict');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');
const test = require('node:test');
const { ZLinkFrameworkRelocationOutcome } = require('../../packages/framework/dist');
const { connectTestRedis } = require('./helpers/test-redis');
const { startChild, stopChild, deletePrefix } = require('./helpers/native-process-harness')(
  path.join(__dirname, 'fixtures', 'actor-direct-route-process.js')
);

// The request outcome inside the commit-to-publication window follows the
// authority-generation rule decided in #4520; this file pins only Find.
test(
  'public Find returns the current Ready authority during relocation staging',
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
    const redis = await connectTestRedis(t);
    if (redis === undefined) {
      if (!process.env.ZLINK_NODE_BOOTSTRAP_FLOW_DIR)
        fs.rmSync(flowDirectory, { recursive: true, force: true });
      return;
    }
    let next;
    let relocation;
    t.after(async () => {
      if (next !== undefined) await next.command('resumePublication');
      if (relocation !== undefined) await relocation;
      await Promise.all(children.map(stopChild));
      await deletePrefix(redis, keyPrefix);
      if (redis.isOpen) await redis.quit();
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
    // A successful source can report before the target's event arrives; only a source that
    // ends without relocating ends the wait.
    const held = target.waitForEvent('publication-held');
    await Promise.race([
      held,
      relocation.then((result) => {
        if (result.outcome !== ZLinkFrameworkRelocationOutcome.Relocated)
          assert.fail(`relocation ended before publication-held: ${JSON.stringify(result)}`);
        return held;
      })
    ]);
    assert.equal((await relocation).outcome, ZLinkFrameworkRelocationOutcome.Relocated);
    const current = await target.command('findActor');
    assert.equal(
      current?.nodeRid,
      nextRid,
      'local staging must not hide the current Ready Store authority'
    );
    assert.equal(current.objectGeneration, '1');
  }
);
