'use strict';

const assert = require('node:assert/strict');
const test = require('node:test');
const {
  ZLinkActorRuntimeOptionsFactory
} = require('../../packages/framework/dist/runtime/host/actor-runtime-options-factory');
const {
  MeshRouterResolver
} = require('../../packages/framework/dist/runtime/host/mesh-router-resolver');

test('bound Session projection follows mesh route changes with the same registry binding', () => {
  const actor = { actorId: 'player', generation: 1n, nodeRid: 'actor-node' };
  const binding = Object.freeze({
    actor,
    sessionOwnerNodeRid: 'session-node',
    sessionRid: 'session',
    bindingGeneration: 1n
  });
  const spotNodes = new Map([['mesh-a', { router: {} }]]);
  const registration = { spotNodes, routeChannels: new Map() };
  const resolver = new MeshRouterResolver(registration);
  const factory = new ZLinkActorRuntimeOptionsFactory({
    registration,
    createLocationSpotRouteResolver: () => undefined,
    locationLifecycle: () => undefined,
    shutdownSignal: () => undefined,
    primaryMeshNodeOrUndefined: () => ({ actorSessionBinding: () => binding }),
    boundSessionRoute: resolver.remoteBoundSessionTargetForSource.bind(resolver)
  });
  const project = factory.createActorManagerOptions().projectBoundSession;
  const first = project(actor);
  assert.equal(first.routerChannelId, 'mesh-a');

  spotNodes.set('mesh-b', spotNodes.get('mesh-a'));
  spotNodes.delete('mesh-a');
  const next = project(actor);
  assert.equal(next.routerChannelId, 'mesh-b');
  assert.equal(next.targetNodeRid, 'session-node');
  assert.equal(next.bindingGeneration, first.bindingGeneration);
  assert.strictEqual(project(actor), next);

  spotNodes.delete('mesh-b');
  assert.strictEqual(project(actor), null);
});
