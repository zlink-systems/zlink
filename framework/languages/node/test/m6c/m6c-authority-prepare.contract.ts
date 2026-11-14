import assert from 'node:assert/strict';
import {
  ZLinkFrameworkException,
  ZLinkFrameworkErrorKind
} from '../../packages/framework/src/contracts/Errors/ZLinkFrameworkException';
import { test } from 'node:test';
import { setImmediate as turn } from 'node:timers/promises';
import { RawServiceMeshRuntime } from '../../packages/framework/src/runtime/foundation/raw-service-mesh-runtime';
import {
  ApplicationJobQueue,
  resolveApplicationJobQueueConfiguration
} from '../../packages/framework/src/runtime/host/application-job-queue';
import { ZLinkNodeRawMeshBackend } from '../../packages/framework/src/runtime/backend/node/node-raw-mesh-backend';
import { ZLinkHostServiceRelocationRuntime } from '../../packages/framework/src/runtime/host/service-relocation-host-runtime';
import { SubmitResult } from '../../packages/framework/src/runtime/backend/runtime-values';
import { RequestRuntime } from '../../packages/framework/src/runtime/foundation/request-runtime';
import { SERVICE_WIRE_REQUIRED_CAPABILITY } from '../../packages/framework/src/runtime/foundation/service-wire-constants.generated';
import { M6aServiceWireCommand } from '../../packages/framework/src/runtime/foundation/service-wire-m6a-codec';
import type { ServiceNodeDescriptor } from '../../packages/framework/src/runtime/foundation/service-topology-registry';
import type { ServiceMaintenanceRelocationPrepare } from '../../packages/framework/src/runtime/foundation/service-stateful-wire-codec';

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
const prepare: ServiceMaintenanceRelocationPrepare = {
  kind: 'prepare',
  deadlineUnixMs: 1700000000000n,
  relocation: { high: 7n, low: 1n },
  targetAttemptGeneration: 1n,
  coordinator: {
    ownerId: 'source-owner',
    leaseGeneration: 3n,
    nodeRid: 'source',
    nodeGeneration: 7n,
    expectedAuthorityStoreVersion: 'v1'
  },
  target: {
    nodeRid: 'target',
    nodeGeneration: 11n,
    ownerId: 'target-owner',
    ownerLeaseGeneration: 13n
  },
  initiatorRole: 'source',
  object: {
    kind: 'actor',
    actorId: 'actor',
    objectGeneration: 3n,
    expectedAuthorityOwnerGeneration: 5n
  },
  sourceNodeRid: 'source',
  sourceNodeGeneration: 7n,
  payloadTotalLength: 0n,
  payloadChunkCount: 0,
  payloadChecksumCrc32c: 0,
  applicationVersion: 1n
};
async function fixture() {
  const incoming: unknown[] = [];
  const submissions: unknown[] = [];
  const router = {
    setRoutingId() {},
    setReceiveFlowState() {},
    bind() {},
    connectToRoutingId() {},
    connect() {},
    disconnect() {},
    disconnectRid() {},
    close() {},
    submitSend: () => ({ result: SubmitResult.Ok, admitted: Promise.resolve() }),
    localEndpoint: () => 'inproc://source',
    receive: () => incoming.shift(),
    send: async () => {},
    routesSnapshot: () => [{ routingId: 'target', routeGeneration: 9n }],
    connectionAttemptsSnapshot: () => [],
    request: (_rid: string, parts: Uint8Array[]) => {
      submissions.push(parts);
      return Promise.resolve();
    }
  };
  const raw = new RawServiceMeshRuntime({
    descriptor: descriptor('source'),
    applicationJobQueue: new ApplicationJobQueue(resolveApplicationJobQueueConfiguration()),
    bindingPort: { createHost: () => ({ createRouter: () => router, close() {} }) } as never
  });
  raw.start();
  raw.connectPeerByRoutingId('inproc://target', 'target', 'default', 7n);
  await raw.observeSelectedRoutes();
  assert.equal(
    raw.admitPeer(
      descriptor('target'),
      9n,
      performance.now(),
      { lifecycleGeneration: 7n },
      M6aServiceWireCommand.admit
    ),
    'admitted'
  );
  raw.connectPeerByRoutingId('inproc://target', 'target', 'default', 11n);
  const backend = new ZLinkNodeRawMeshBackend(
    'authority',
    'source',
    {} as never,
    undefined,
    undefined,
    undefined,
    undefined,
    { raw }
  );
  const requests = new RequestRuntime();
  const host = new ZLinkHostServiceRelocationRuntime({
    requests,
    meshNode: () => backend
  } as never);
  return {
    raw,
    backend,
    host,
    requests,
    submissions,
    close() {
      requests.close();
      raw.close();
    }
  };
}
for (const path of ['host PREPARE', 'backend PREPARE'] as const) {
  for (const ending of ['removed', 'timeout'] as const) {
    test(`${path}: authority G waits past older admitted peer until ${ending}`, async () => {
      const f = await fixture();
      try {
        const timeoutMs = ending === 'removed' ? 1000 : 30;
        let settled = false;
        const pending =
          path === 'host PREPARE'
            ? f.host.sendControl(
                'authority',
                'target',
                { ...prepare, deadlineUnixMs: BigInt(Date.now() + timeoutMs) },
                undefined,
                performance.now() + timeoutMs
              )
            : f.backend.requestInfrastructureControlFrames('target', [Buffer.from([1])], {
                timeoutMs,
                targetNodeGeneration: 11n
              } as never);
        void pending.then(
          () => {
            settled = true;
          },
          () => {
            settled = true;
          }
        );
        await turn();
        assert.equal(settled, false, 'PREPARE must remain pending for the authority generation');
        assert.equal(f.submissions.length, 0, 'older admitted generation must receive no PREPARE');
        if (ending === 'removed') f.raw.disconnectPeer('inproc://target', 'target');
        else if (path === 'host PREPARE')
          f.requests.registry.expire(performance.now() + 1000, Infinity);
        else f.raw.expireOperations(performance.now() + 1000, Infinity);
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

test('relocation READY remains one-way without request admission', async () => {
  const requests = new RequestRuntime();
  let actualOptions: { targetNodeGeneration?: bigint } | undefined;
  const stopped = new Error('captured READY options');
  const host = new ZLinkHostServiceRelocationRuntime({
    requests,
    meshNode: () => ({
      requestInfrastructureControlFrames(
        _target: unknown,
        _frames: unknown,
        options: typeof actualOptions
      ) {
        actualOptions = options;
        return Promise.reject(stopped);
      }
    })
  } as never);
  try {
    await assert.rejects(
      host.sendControl('authority', 'target', {
        kind: 'ready',
        relocation: prepare.relocation,
        targetAttemptGeneration: prepare.targetAttemptGeneration,
        coordinator: prepare.coordinator,
        target: prepare.target,
        object: prepare.object,
        senderRole: 'target'
      }),
      /Relocation ready is one-way and has no response key/
    );
    assert.equal(actualOptions, undefined, 'one-way READY must not enter request admission');
  } finally {
    requests.close();
  }
});
