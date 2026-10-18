const assert = require('node:assert/strict');
const test = require('node:test');
const framework = require('../../packages/framework/dist/internal');
const { RequestResult } = require('../../packages/framework/dist/runtime/backend/runtime-values');
const {
  ServiceStatefulRuntime
} = require('../../packages/framework/dist/runtime/foundation/service-stateful-runtime');
const wire = require('../../packages/framework/dist/runtime/foundation/service-stateful-wire-codec');

test('command 44 cross-node route preserves the binding for the next Actor Join command 42', async (t) => {
  const actorId = 'actor-consecutive-join';
  let nativeBinds = 0;
  let lookupActor;
  const serviceRuntime = new ServiceStatefulRuntime(
    {
      observePeerRoute: () => ({ generation: 1n, ready: true }),
      observePeerRouteChanged: () => () => {},
      observePeerConnectionIntentRemoved: () => () => {},
      setServiceIngress() {},
      async requestService(_target, parts) {
        const request = wire.decodeStatefulHeader(parts[0]);
        return [
          wire.encodeStatefulReply(request.correlation, RequestResult.Ok, 0, {
            kind: 'streamBind',
            bindingGeneration: BigInt(++nativeBinds),
            authorityOwnerGeneration: request.actor.authorityOwnerGeneration
          })
        ];
      }
    },
    'session-owner',
    4n
  );
  t.after(() => serviceRuntime.close());
  const service = {
    status: () => ({ state: 2 }),
    lookupActor(nodeRid, id) {
      lookupActor = { nodeRid, actorId: id, generation: 5n };
      return { kind: 'lookup' };
    },
    bindActor(sessionRid, actor, timeoutMs, onReplaced, authority) {
      return serviceRuntime.bindSession(
        sessionRid,
        actor,
        timeoutMs,
        () => true,
        onReplaced,
        authority
      );
    },
    bindings: (sessionRid) => serviceRuntime.sessionBindings(sessionRid)
  };
  const stream = new framework.ZLinkManagedStream(
    {
      send: () => true,
      async submit() {},
      disconnectPeer() {},
      recv() {}
    },
    'session',
    undefined,
    service,
    {
      async submit(submit) {
        const pending = submit();
        const result = await pending.promise;
        return {
          terminalResult: result?.terminalResult ?? 0,
          failureErrno: result?.failureCode ?? 0,
          kindData:
            pending.kind === 'lookup'
              ? { kind: 'actorLookupCompletion', location: { actor: lookupActor } }
              : undefined,
          parts: []
        };
      }
    }
  );
  const host = new framework.ZLinkFrameworkRuntimeHost({
    registration: framework.createFrameworkRegistration()
  });
  const relay = host.boundSessionRelay.boundSessions;
  t.after(() => relay.clearServiceWireSessionRelocations());
  const context = host.streamBindingRuntime.createSessionContext(stream);
  const actor = await context.actors.bind({
    actorId,
    objectGeneration: 5n,
    nodeRid: 'source',
    meshName: 'play.route',
    ownershipGeneration: 11n,
    ownerLeaseGeneration: 13n,
    ownerNodeGeneration: 2n
  });
  const bindingGeneration = actor.ref.bindingGeneration;
  assert.equal(bindingGeneration, 1n);
  const bindingToken = actor.bindingToken;
  const initial = {
    targetNodeRid: 'target',
    targetNodeGeneration: 4n,
    relocation: { high: 7n, low: 9n },
    coordinator: {
      ownerId: 'source-owner',
      leaseGeneration: 13n,
      nodeRid: 'source',
      nodeGeneration: 2n,
      expectedAuthorityStoreVersion: 'store-v17'
    },
    senderRole: 'source',
    actor: {
      actor: { actorId, generation: 5n, nodeRid: 'source' },
      targetNodeGeneration: 2n,
      authorityOwnerGeneration: 11n,
      ownerLeaseGeneration: 13n
    },
    session: {
      sessionOwnerNodeRid: 'session-owner',
      sessionOwnerNodeGeneration: 4n,
      sessionOwnerId: 'session-owner-id',
      sessionOwnerLeaseGeneration: 8n,
      sessionRid: 'session',
      bindingGeneration
    }
  };
  // Use the decoded command bodies at the real Session owner ingress. Native
  // bind and its returned generation above run through ServiceStatefulRuntime.
  const seal = async (value) =>
    relay.receiveServiceWireSessionRelocationSeal(
      wire.decodeSessionRelocationSeal(wire.encodeSessionRelocationSeal(value))
    );
  const route = {
    relocation: initial.relocation,
    coordinator: initial.coordinator,
    senderRole: 'target',
    actor: { actorId, generation: 5n, nodeRid: 'target' },
    session: initial.session,
    route: {
      action: 'commit',
      previousAuthorityOwnerGeneration: 11n,
      targetAuthorityOwnerGeneration: 12n,
      targetOwnerLeaseGeneration: 14n,
      targetNodeRid: 'target',
      targetNodeGeneration: 4n
    }
  };
  assert.equal((await seal(initial)).sealResult, 'sealed');
  await relay.receiveServiceWireSessionRelocationRoute(
    wire.decodeSessionRelocationRoute(wire.encodeSessionRelocationRoute(route))
  );
  const nextJoin = {
    ...initial,
    targetNodeRid: 'source',
    targetNodeGeneration: 2n,
    relocation: { high: 17n, low: 19n },
    coordinator: {
      ...initial.coordinator,
      ownerId: 'target-owner',
      nodeRid: 'target',
      nodeGeneration: 4n,
      leaseGeneration: 14n
    },
    actor: {
      actor: { actorId, generation: 5n, nodeRid: 'target' },
      targetNodeGeneration: 4n,
      authorityOwnerGeneration: 12n,
      ownerLeaseGeneration: 14n
    }
  };
  // The target retained the physical binding from relocation. Its next Join
  // carries that exact fence without waiting for a separate bind confirmation.
  t.diagnostic(
    JSON.stringify({
      nativeBinds,
      expectedGeneration: String(bindingGeneration),
      currentGeneration: String(actor.ref.bindingGeneration)
    })
  );
  await assert.rejects(
    seal({
      ...nextJoin,
      relocation: { high: 23n, low: 29n },
      session: { ...nextJoin.session, bindingGeneration: bindingGeneration + 10n }
    }),
    /command 42 was fenced by its exact Session route/
  );
  assert.equal((await seal(nextJoin)).sealResult, 'sealed');
  assert.equal(await context.actors.find(actorId), actor);
  assert.equal(actor.bindingToken, bindingToken);
  assert.equal(actor.ref.bindingGeneration, bindingGeneration);
  assert.equal(String(actor.ref.nodeRid), 'target');
  assert.equal(nativeBinds, 1);
});
