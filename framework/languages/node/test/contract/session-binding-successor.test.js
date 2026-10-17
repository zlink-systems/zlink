const assert = require('node:assert/strict');
const test = require('node:test');
const {
  ZLinkActorSessionBindingRegistry
} = require('../../packages/framework/dist/runtime/streams/actor-session-binding-registry');

const context = (routingId) => ({ routingId, bindLocal() {}, unbindLocal() {} });
const actor = (bindingGeneration) => ({
  actorId: 'actor',
  ref: { actorId: 'actor', objectGeneration: 1n, bindingGeneration }
});

for (const sameContext of [false, true]) {
  test(`successor retires predecessor seal, same context ${sameContext}`, async () => {
    const registry = new ZLinkActorSessionBindingRegistry();
    const oldContext = context('old-session');
    const oldActor = actor(1n);
    const fence = { objectGeneration: 1n, bindingGeneration: 1n };
    await registry.bind(oldContext, oldActor, 'old-token', fence);
    await registry.seal('actor', 'old-seal', fence);
    await registry.replace(await registry.route('actor'), oldContext, oldActor, 'old-token');
    assert.equal((await registry.route('actor')).sealId, 'old-seal');
    assert.equal((await registry.route('actor')).authorityFence, fence);

    // The old ingress is held before replacement; its rejection is observed
    // immediately so retirement cannot become an unhandled rejection.
    let ingressHeld;
    const held = new Promise((resolve) => {
      ingressHeld = resolve;
    });
    const waitForSealRelease = registry.waitForSealReleaseCore.bind(registry);
    registry.waitForSealReleaseCore = (...args) => {
      const wait = waitForSealRelease(...args);
      ingressHeld();
      return wait;
    };
    const oldIngress = assert.rejects(registry.acceptWhenReady('actor', 'old-token'));
    await held;
    const successor = sameContext ? oldContext : context('new-session');
    await registry.replace(await registry.route('actor'), successor, actor(2n), 'new-token');
    const route = await registry.route('actor');
    assert.equal(route.sealId, undefined);
    assert.equal(route.authorityFence, undefined);
    assert.equal(route.sessionRelocationResult, undefined);
    assert.equal(route.activeFrames.count, 0);
    await registry.accept('actor', 'new-token');
    await oldIngress;
    await registry.unbind('actor', oldContext, 'old-token');
    assert.equal((await registry.route('actor')).context, successor);
    await registry.cleanup(successor);
  });
}

test('route refresh on the same physical Session keeps its seal across context wrappers', async () => {
  const registry = new ZLinkActorSessionBindingRegistry();
  const oldContext = context('session');
  const oldActor = actor(1n);
  const fence = { objectGeneration: 1n, bindingGeneration: 1n };
  await registry.bind(oldContext, oldActor, 'token', fence);
  await registry.seal('actor', 'seal', fence);
  const oldRoute = await registry.route('actor');
  await registry.replaceAndReleaseSeal(oldRoute, context('session'), oldActor, 'token', 'seal');
  const refreshed = await registry.route('actor');
  assert.equal(refreshed.activeFrames, oldRoute.activeFrames);
  assert.equal(refreshed.authorityFence, fence);
  assert.equal(refreshed.sealId, undefined);
  await registry.cleanup(refreshed.context);
});

test('a late predecessor seal release cannot release the successor seal', async () => {
  const registry = new ZLinkActorSessionBindingRegistry();
  const oldContext = context('old-session');
  await registry.bind(oldContext, actor(1n), 'old-token');
  await registry.seal('actor', 'old-seal', { objectGeneration: 1n, bindingGeneration: 1n });
  await registry.replace(
    await registry.route('actor'),
    context('new-session'),
    actor(2n),
    'new-token'
  );
  await registry.sealAndWait('actor', 'new-seal', { objectGeneration: 1n, bindingGeneration: 2n });
  assert.equal(await registry.abortSeal('actor', 'old-seal'), false);
  await registry.unbind('actor', oldContext, 'old-token');
  assert.equal((await registry.route('actor')).sealId, 'new-seal');
  assert.equal(await registry.abortSeal('actor', 'new-seal'), true);
  await registry.cleanup((await registry.route('actor')).context);
});
