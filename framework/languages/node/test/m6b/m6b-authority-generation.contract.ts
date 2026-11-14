import assert from 'node:assert/strict';
import { test } from 'node:test';
import {
  ZLinkFrameworkException,
  ZLinkFrameworkErrorKind
} from '../../packages/framework/src/contracts/Errors/ZLinkFrameworkException';
import { RawServiceMeshRuntime } from '../../packages/framework/src/runtime/foundation/raw-service-mesh-runtime';
import { ServiceStatefulRuntime } from '../../packages/framework/src/runtime/foundation/service-stateful-runtime';
import {
  ApplicationJobQueue,
  resolveApplicationJobQueueConfiguration
} from '../../packages/framework/src/runtime/host/application-job-queue';
import {
  RequestResult,
  SubmitResult
} from '../../packages/framework/src/runtime/backend/runtime-values';
import { SERVICE_WIRE_REQUIRED_CAPABILITY } from '../../packages/framework/src/runtime/foundation/service-wire-constants.generated';
import {
  encodeRouteMeshAdmission,
  M6aServiceWireCommand
} from '../../packages/framework/src/runtime/foundation/service-wire-m6a-codec';
import type { ServiceNodeDescriptor } from '../../packages/framework/src/runtime/foundation/service-topology-registry';
import type { RawServiceIngressRecord } from '../../packages/framework/src/runtime/foundation/raw-service-mesh-runtime';

const payload = {
  packetName: 'Authority',
  contentType: 'application/json',
  payload: Buffer.from('{}')
};
const turn = () => new Promise<void>((resolve) => setImmediate(resolve));
function descriptor(rid: string): ServiceNodeDescriptor {
  return {
    meshName: 'authority',
    nodeRoutingId: rid,
    lifecycleGeneration: 7n,
    descriptorRevision: 1n,
    advertisedEndpoint: `inproc://${rid}`,
    channels: [],
    state: 'serving',
    securityIdentity: 'default',
    applicationVersion: 1n,
    protocolCapabilities: [SERVICE_WIRE_REQUIRED_CAPABILITY],
    objectRole: 'server',
    placementWeight: 100,
    activeCapacityLimit: 100,
    pendingCapacityLimit: 10,
    activeCapacityUsed: 0,
    pendingCapacityUsed: 0
  };
}
async function fixture(discovery: 'newer' | 'older' | 'absent' = 'newer') {
  const incoming: unknown[] = [];
  const submissions: readonly Uint8Array[][] = [];
  const router = {
    setRoutingId() {},
    setReceiveFlowState() {},
    bind() {},
    connectToRoutingId() {},
    connect() {},
    disconnect() {},
    disconnectRid() {},
    close() {},
    submitSend: () => ({ result: SubmitResult.Ok }),
    localEndpoint: () => 'inproc://source',
    receive: () => incoming.shift(),
    send: async () => {},
    routesSnapshot: () => [{ routingId: 'target', routeGeneration: 9n }],
    connectionAttemptsSnapshot: () => [],
    request: (_rid: string, parts: Uint8Array[]) => {
      (submissions as Uint8Array[][]).push(parts);
      return Promise.resolve();
    }
  };
  const raw = new RawServiceMeshRuntime({
    descriptor: descriptor('source'),
    applicationJobQueue: new ApplicationJobQueue(resolveApplicationJobQueueConfiguration()),
    bindingPort: { createHost: () => ({ createRouter: () => router, close() {} }) } as never
  });
  raw.start();
  if (discovery !== 'absent')
    raw.connectPeerByRoutingId('inproc://target', 'target', 'default', 7n);
  await raw.observeSelectedRoutes();
  incoming.push({
    sourceRid: 'target',
    routeGeneration: 9n,
    parts: [encodeRouteMeshAdmission(M6aServiceWireCommand.admit, descriptor('target'))],
    close() {}
  });
  assert.equal(await raw.pumpOne(), 'infrastructure');
  if (discovery === 'newer')
    raw.connectPeerByRoutingId('inproc://target', 'target', 'default', 11n);
  const runtime = new ServiceStatefulRuntime(raw, 'source', 7n);
  const spot = {
    spot: { spotId: 'room', generation: 3n },
    targetNodeRid: 'target',
    targetNodeGeneration: 11n,
    authorityOwnerGeneration: 5n,
    ownerLeaseGeneration: 13n,
    storeVersion: 'v1'
  };
  const actor = { nodeRid: 'target', actorId: 'actor', generation: 3n };
  runtime.rememberSpotRoute(spot);
  runtime.rememberActorRoute({
    actor,
    targetNodeGeneration: 11n,
    authorityOwnerGeneration: 5n,
    ownerLeaseGeneration: 13n
  });
  const instance = {
    targetNodeRid: 'target',
    targetNodeGeneration: 11n,
    targetSpotId: 'instance',
    objectGeneration: 3n,
    ownerId: 'owner',
    authorityOwnerGeneration: 5n,
    leaseGeneration: 13n,
    storeVersion: 'v1'
  };
  const reservation = {
    reservationId: 'reserve',
    expectedStoreVersion: 'v1',
    objectGeneration: 3n,
    authorityOwnerGeneration: 5n,
    targetNodeRid: 'target',
    targetNodeGeneration: 11n,
    targetOwnerId: 'owner',
    targetOwnerLeaseGeneration: 13n,
    pendingCapacityDelta: 1
  };
  return {
    raw,
    runtime,
    submissions,
    spot,
    actor,
    instance,
    reservation,
    start(path: string, timeoutMs: number): Promise<unknown> {
      if (path === 'Spot')
        return runtime.requestToSpot('source-room', spot, payload, timeoutMs).promise;
      if (path === 'Actor') return runtime.requestToActor(actor, payload, timeoutMs).promise;
      if (path === 'Instance Spot')
        return runtime.requestToInstanceSpot(instance, payload, timeoutMs).promise;
      if (path === 'missing Instance Spot')
        return runtime.requestToMissingInstanceSpot(
          {
            ...instance,
            stableType: 'instance',
            descriptorVersion: 'v1',
            targetMeshName: 'authority'
          },
          payload,
          timeoutMs
        ).promise;
      if (path === 'Actor join')
        return runtime.joinActor(actor, 'target', spot.spot, 3n, undefined, timeoutMs).promise;
      if (path === 'Actor create')
        return runtime.requestActorCreate(
          'target',
          {
            actorId: 'created',
            stableType: 'actor',
            sourceNodeRid: 'source',
            sourceNodeGeneration: 7n,
            operation: { high: 7n, low: 1n },
            reservation,
            deadlineUnixMs: BigInt(Date.now() + timeoutMs)
          },
          timeoutMs
        );
      if (path === 'User Spot create')
        return runtime.requestUserSpotCreate(
          'target',
          {
            spotId: 'created',
            stableType: 'spot',
            sourceNodeRid: 'source',
            sourceNodeGeneration: 7n,
            reservation,
            deadlineUnixMs: BigInt(Date.now() + timeoutMs)
          },
          timeoutMs
        );
      return runtime.requestUserSpotClose(
        'target',
        {
          sourceNodeRid: 'source',
          sourceNodeGeneration: 7n,
          target: {
            spotId: 'room',
            objectGeneration: 3n,
            targetNodeRid: 'target',
            targetNodeGeneration: 11n,
            authorityOwnerGeneration: 5n,
            expectedStoreVersion: 'v1'
          },
          deadlineUnixMs: BigInt(Date.now() + timeoutMs)
        },
        timeoutMs
      );
    },
    close() {
      runtime.close();
      raw.close();
    }
  };
}
for (const path of [
  'Spot',
  'Actor',
  'Instance Spot',
  'missing Instance Spot',
  'Actor join',
  'Actor create',
  'User Spot create',
  'User Spot close'
]) {
  for (const discovery of ['newer', 'older'] as const) {
    for (const ending of ['removed', 'timeout']) {
      test(`${path}: authority G waits past older admitted peer until ${ending} (descriptor ${discovery})`, async () => {
        const f = await fixture(discovery);
        try {
          let settled = false;
          const pending = f.start(path, ending === 'removed' ? 1000 : 30);
          void pending.then(
            () => {
              settled = true;
            },
            () => {
              settled = true;
            }
          );
          await turn();
          assert.equal(settled, false, 'authority request must remain pending');
          assert.equal(
            f.submissions.length,
            0,
            'older admitted generation must receive no request'
          );
          if (ending === 'removed') f.raw.disconnectPeer('inproc://target', 'target');
          else f.runtime.expireOperations(performance.now() + 1000, Number.POSITIVE_INFINITY);
          await assert.rejects(
            pending,
            (error: unknown) =>
              error instanceof ZLinkFrameworkException &&
              error.kind ===
                (ending === 'removed'
                  ? ZLinkFrameworkErrorKind.Unavailable
                  : ZLinkFrameworkErrorKind.DeadlineExceeded)
          );
          assert.equal(f.submissions.length, 0);
        } finally {
          f.close();
        }
      });
    }
  }
}
test('bare Actor handle request and send are NotFound without authority or peer', async () => {
  const f = await fixture();
  try {
    const actor = { nodeRid: 'absent', actorId: 'bare', generation: 1n };
    assert.equal(await f.runtime.sendToActor(actor, payload), SubmitResult.NotFound);
    assert.equal(
      (await f.runtime.requestToActor(actor, payload, 100).promise).terminalResult,
      RequestResult.NotFound
    );
  } finally {
    f.close();
  }
});

test('authority present without a route keeps one-way send NotConnected', async () => {
  const f = await fixture();
  try {
    assert.equal(f.raw.isPeerRouteReady('target', 11n), false);
    assert.equal(
      await f.runtime.sendToSpot('source-room', f.spot, payload),
      SubmitResult.NotConnected
    );
    assert.equal(await f.runtime.sendToActor(f.actor, payload), SubmitResult.NotConnected);
  } finally {
    f.close();
  }
});

for (const discovery of ['absent', 'newer'] as const) {
  for (const senderGeneration of [7n, 8n]) {
    test(`Instance ingress uses received sender G ${senderGeneration} with ${discovery} discovery`, async () => {
      const f = await fixture(discovery);
      try {
        const ingress = {
          sourceRoutingId: 'target',
          sourceNodeGeneration: senderGeneration
        } as RawServiceIngressRecord;
        const record = {
          activation: 'ready',
          sourceNodeRid: 'target',
          sourceNodeGeneration: 7n,
          route: f.instance
        } as Parameters<ServiceStatefulRuntime['validateInstanceIngress']>[1];
        if (senderGeneration === 7n)
          assert.doesNotThrow(() => f.runtime.validateInstanceIngress(ingress, record));
        else
          assert.throws(
            () => f.runtime.validateInstanceIngress(ingress, record),
            /stale source lifecycle/
          );
      } finally {
        f.close();
      }
    });

    test(`creation ingress uses received sender G ${senderGeneration} with ${discovery} discovery`, async () => {
      const f = await fixture(discovery);
      try {
        const owner = await f.raw.reserveLocalIngress();
        const record = {
          kind: 'actorCreate',
          sourceNodeRid: 'target',
          sourceNodeGeneration: 7n,
          correlation: 1n,
          operation: { high: 7n, low: 1n },
          actorId: 'created',
          stableType: 'actor',
          reservation: { ...f.reservation, targetNodeRid: 'source', targetNodeGeneration: 7n },
          deadlineUnixMs: BigInt(Date.now() + 1000)
        } as const;
        const ingress = {
          sourceRoutingId: 'target',
          sourceNodeGeneration: senderGeneration,
          requestSequence: 1n,
          reply() {},
          parts: [],
          applicationJobOwner: owner
        } as unknown as RawServiceIngressRecord;
        try {
          assert.equal(
            f.runtime['handleUserSpotOperation'](ingress, record),
            senderGeneration === 7n ? 'infrastructure' : 'protocolError'
          );
          await turn();
        } finally {
          owner.close();
        }
      } finally {
        f.close();
      }
    });
  }
}

test('request source acceptance uses ingress G and local runtime G', async () => {
  const f = await fixture('absent');
  try {
    const seen: bigint[] = [];
    f.runtime.setRequestSourceAcceptance((nodeRid, nodeGeneration) => {
      seen.push(nodeGeneration);
      return { nodeRid, nodeGeneration, ownerId: 'owner', leaseGeneration: 1n };
    });
    for (const sourceRoutingId of ['target', 'source']) {
      f.runtime['acceptRequestSource']({
        sourceRoutingId,
        sourceNodeGeneration: 8n
      } as RawServiceIngressRecord);
    }
    assert.deepEqual(seen, [8n, 7n]);
  } finally {
    f.close();
  }
});

test('active bind resolves the received sender G without a discovery expectation', async () => {
  const f = await fixture('absent');
  try {
    let seen: bigint | undefined;
    f.runtime.setSessionOwnerDescriptors({
      local: () => undefined,
      peer: async (_rid, generation) => {
        seen = generation;
        return undefined;
      }
    });
    await f.runtime['replySessionBind'](
      {
        sourceRoutingId: 'target',
        sourceNodeGeneration: 8n,
        requestSequence: 1n,
        reply() {}
      } as unknown as RawServiceIngressRecord,
      { binding: { state: 'active', generation: 1n }, correlation: 1n } as Parameters<
        ServiceStatefulRuntime['replySessionBind']
      >[1]
    );
    assert.equal(seen, 8n);
  } finally {
    f.close();
  }
});

for (const senderGeneration of [7n, 8n]) {
  test(`replacement notice checks received sender G ${senderGeneration} without discovery`, async () => {
    const f = await fixture('absent');
    try {
      let accepted = false;
      f.runtime.setSessionOwnerDescriptors({
        local: () => {
          accepted = true;
          return undefined;
        },
        peer: async () => undefined
      });
      f.runtime['handleBoundSessionReplaced'](
        {
          sourceRoutingId: 'target',
          sourceNodeGeneration: senderGeneration
        } as RawServiceIngressRecord,
        { actorAuthority: { actor: f.actor, targetNodeGeneration: 7n } } as Parameters<
          ServiceStatefulRuntime['handleBoundSessionReplaced']
        >[1]
      );
      assert.equal(accepted, senderGeneration === 7n);
    } finally {
      f.close();
    }
  });

  test(`binding tombstone checks received sender G ${senderGeneration} without discovery`, async () => {
    const f = await fixture('absent');
    try {
      let accepted = false;
      Object.defineProperty(f.runtime.registry, 'binding', {
        value: () => ({
          bindingGeneration: 3n,
          sessionRid: 'session',
          sessionOwnerNodeRid: 'target',
          sessionOwnerNodeGeneration: 7n
        })
      });
      Object.defineProperty(f.runtime, 'enqueueSessionBindingRetirement', {
        value: () => {
          accepted = true;
          return true;
        }
      });
      f.runtime['enqueueSessionBindingTombstone'](
        {
          sourceRoutingId: 'target',
          sourceNodeGeneration: senderGeneration,
          requestSequence: 1n,
          reply() {}
        } as unknown as RawServiceIngressRecord,
        {
          actor: { actor: f.actor },
          binding: { state: 'tombstone', retiredGeneration: 3n },
          sessionRid: 'session',
          correlation: 1n
        } as Parameters<ServiceStatefulRuntime['enqueueSessionBindingTombstone']>[1]
      );
      assert.equal(accepted, senderGeneration === 7n);
    } finally {
      f.close();
    }
  });
}
