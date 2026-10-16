const assert = require('node:assert/strict');
const test = require('node:test');
const {
  ZLinkActorSessionBindingRegistry
} = require('../../packages/framework/dist/runtime/streams/actor-session-binding-registry');
const { decodeRoutingId } = require('../../packages/framework/dist/runtime/routing-id');

for (const explicitIdentity of [false, true]) {
  test(`Session route selects byte identity, explicit identity ${explicitIdentity}`, async () => {
    const sessionRid = decodeRoutingId('display-one', '00000001');
    const context = { routingId: sessionRid, bindLocal() {}, unbindLocal() {} };
    const actor = { actorId: 'actor', ref: { actorId: 'actor', bindingGeneration: 3n } };
    const routes = new ZLinkActorSessionBindingRegistry();
    await routes.bind(
      context,
      actor,
      'binding',
      undefined,
      explicitIdentity ? sessionRid : undefined
    );
    const selected = await routes.routeForSessionBinding(
      'actor',
      decodeRoutingId('different-display', '00000001'),
      3n
    );
    assert.equal(
      selected?.context,
      context,
      'equal RID bytes select the binding regardless of display'
    );
    assert.equal(
      await routes.routeForSessionBinding('actor', decodeRoutingId('display-one', '00000002'), 3n),
      undefined,
      'display collision must not select another Session'
    );
    assert.equal(await routes.routeForSessionBinding('actor', 'display-one', 3n), undefined);
    assert.equal(
      await routes.routeForSessionBinding('actor', sessionRid, 4n),
      undefined,
      'binding generation remains exact'
    );
  });
}

test('peer readiness compares opaque Node RID bytes for local exclusion and admitted peer', () => {
  const framework = require('../../packages/framework/dist/internal');
  const local = {
    routingId: decodeRoutingId('same-display', '00000001'),
    lifecycleGeneration: 1n,
    applicationVersion: 1n
  };
  const descriptor = {
    rid: decodeRoutingId('same-display', '00000002'),
    lifecycleGeneration: 1n,
    applicationVersion: 1n,
    state: framework.ZLinkFrameworkRuntimeState.Serving,
    objectRole: 'server'
  };
  const peer = {
    routingId: decodeRoutingId('different-display', '00000002'),
    lifecycleGeneration: 1n,
    state: 3
  };
  assert.equal(framework.hasExactPeerReadiness([descriptor], local, [peer]), true);
  assert.equal(
    framework.hasExactPeerReadiness(
      [{ ...descriptor, rid: decodeRoutingId('different-display', '00000001') }],
      local,
      [{ ...peer, routingId: decodeRoutingId('different-display', '00000001') }]
    ),
    false
  );
  assert.equal(
    framework.hasExactPeerReadiness([descriptor], local, [
      { ...peer, routingId: decodeRoutingId('same-display', '00000003') }
    ]),
    false
  );
});

test('Actor tenure lookup uses the byte identity in its canonical owner fence', () => {
  const {
    ZLinkActorHandoffCoordinator
  } = require('../../packages/framework/dist/runtime/actors/actor-handoff');
  const coordinator = new ZLinkActorHandoffCoordinator({
    currentOwnerFence: () => ({
      nodeRid: 'display-one',
      nodeRidHex: '00000001',
      authorityOwnerGeneration: '4'
    })
  });
  const actor = { actorId: 'actor', nodeRid: decodeRoutingId('different-display', '00000001') };
  assert.equal(coordinator.tenureAuthorityOwnerGeneration('actor', actor), 4n);
  assert.equal(
    coordinator.tenureAuthorityOwnerGeneration('actor', {
      ...actor,
      nodeRid: decodeRoutingId('display-one', '00000002')
    }),
    undefined
  );
});
