const assert = require('node:assert/strict');
const { spawnSync } = require('node:child_process');
const test = require('node:test');
const {
  DefaultZLinkActorClient,
  ZLinkSpotKind
} = require('../../packages/framework/dist/internal');
const identityModule =
  require.resolve('../../packages/framework/dist/runtime/actors/actor-message-follow-context');
const { actorMessageFollowContext, createMessageFollowId } = require(identityModule);

function assertIdentity(id) {
  assert.match(id, /^[0-9a-f]{32}$/);
  assert.notEqual(id, '0'.repeat(32));
}

test('Actor Message Follow allocates 100,000 unique IDs in one process', () => {
  const ids = new Set();
  for (let index = 0; index < 100_000; index++) {
    const id = createMessageFollowId();
    assertIdentity(id);
    ids.add(id);
  }
  assert.equal(ids.size, 100_000);
});

test('two Actor client runtimes share the allocator for sends and all three request IDs', async () => {
  const actorId = 'actor';
  const actorRef = { actorId, nodeRid: 'node-a', objectGeneration: 1n, meshName: 'mesh' };
  const route = {
    actorRef,
    meshName: actorRef.meshName,
    ownerNodeGeneration: 1n,
    ownerId: 'owner-a',
    ownerLeaseGeneration: 1n,
    authorityOwnerGeneration: 1n,
    spotKind: ZLinkSpotKind.Entry,
    spotId: 'entry',
    spotGeneration: 1n
  };
  const ids = [];
  const makeClient = () =>
    new DefaultZLinkActorClient({
      locationResolver: () => ({
        async resolveDirectActorRoute() {
          return { kind: 'ready', route };
        }
      }),
      handoffCapture(_mesh, _actorId, _parts, request, routedActor) {
        const context = actorMessageFollowContext(routedActor);
        assert.equal(context.request, request);
        ids.push(context.operationId);
        if (request) ids.push(context.correlationId, context.replyRouteId);
        return Promise.resolve('reply');
      }
    });
  const clients = [makeClient(), makeClient()];
  await Promise.all(
    clients.map(async (client) => {
      await client.sendToActor(actorId, 'notice').packetName('ActorNotice').submit();
      assert.equal(
        await client.requestToActor(actorId, 'query').packetName('ActorQuery').submit(),
        'reply'
      );
    })
  );
  assert.equal(ids.length, 8);
  ids.forEach(assertIdentity);
  assert.equal(new Set(ids).size, ids.length);
  const prefix = ids[0].slice(0, 16);
  assert.ok(ids.every((id) => id.startsWith(prefix)));
});

test('each process seeds the Actor Message Follow counter once and issues distinct IDs', () => {
  const script = `
    const crypto = require('node:crypto');
    const randomBytes = crypto.randomBytes;
    let draws = 0;
    crypto.randomBytes = (...args) => {
      draws++;
      return randomBytes(...args);
    };
    const { createMessageFollowId } = require(process.argv[1]);
    const startupDraws = draws;
    const ids = Array.from({ length: 4096 }, () => createMessageFollowId());
    process.stdout.write(JSON.stringify({ ids, startupDraws, draws }));
  `;
  const run = () => {
    const child = spawnSync(process.execPath, ['-e', script, identityModule], { encoding: 'utf8' });
    assert.ifError(child.error);
    assert.equal(child.status, 0, child.stderr);
    return JSON.parse(child.stdout);
  };
  const first = run();
  const second = run();
  for (const result of [first, second]) {
    assert.equal(result.draws - result.startupDraws, 0, 'issuing IDs should draw no entropy');
    assert.equal(result.startupDraws, 1);
    const prefix = result.ids[0].slice(0, 16);
    result.ids.forEach((id, index) => {
      assertIdentity(id);
      assert.equal(id.slice(0, 16), prefix);
      assert.equal(BigInt(`0x${id.slice(16)}`), BigInt(index + 1));
    });
  }
  assert.equal(new Set([...first.ids, ...second.ids]).size, 8192);
});
