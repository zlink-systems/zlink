const assert = require('node:assert/strict');
const test = require('node:test');

const {
  ApplicationJobQueue,
  resolveApplicationJobQueueConfiguration
} = require('../../packages/framework/dist/runtime/host/application-job-queue');
const {
  runWithApplicationJobPermit
} = require('../../packages/framework/dist/runtime/application-jobs/application-job-queue-scope');
const { DefaultZLinkSpotManager } = require('../../packages/framework/dist/runtime/spots');
const { ZLinkFrameworkRuntimeHost } = require('../../packages/framework/dist/runtime/host');
const {
  ZLinkBufferMessage
} = require('../../packages/framework/dist/runtime/backend/runtime-message');
const {
  ReceiveKind
} = require('../../packages/framework/dist/runtime/foundation/service-runtime-contracts');
const { SubmitResult } = require('../../packages/framework/dist/runtime/backend/runtime-values');

function deferred() {
  let resolve;
  const promise = new Promise((done) => {
    resolve = done;
  });
  return { promise, resolve };
}

function queueWithOnePermit() {
  return new ApplicationJobQueue(
    resolveApplicationJobQueueConfiguration({ maxQueuedApplicationJobs: 1n }, () => 8n)
  );
}

test('canonical Actor Join returns the ingress permit before waiting for the Location Store', async () => {
  const queue = queueWithOnePermit();
  const store = deferred();
  const entered = deferred();
  const activation = {
    domain: { kind: 'user' },
    spotId: 'spot-a',
    serial: { executeLifecycleOperation: async (callback) => callback() }
  };
  const manager = {
    activations: { resolve: () => activation },
    formalRemoteActorAdmissions: {
      beginProvisional: () => ({ record: {}, created: true })
    },
    options: {
      canonicalActorJoinResolver: () => {
        entered.resolve();
        return store.promise;
      }
    },
    dispatchMeshActorJoinCore: DefaultZLinkSpotManager.prototype.dispatchMeshActorJoinCore
  };
  const request = ZLinkBufferMessage.from('join');
  const record = {
    kindData: {
      kind: 'actorControl',
      currentActor: { actorId: 'actor-a', nodeRid: 'node-a', generation: 1n },
      canonicalActorJoin: { handoffId: 'join-a', actorNodeRid: 'node-a' }
    },
    parts: [request],
    replyFailure: () => SubmitResult.Ok
  };
  const first = await queue.acquire();
  first.markApplicationQueued();
  const dispatch = runWithApplicationJobPermit(first, () =>
    DefaultZLinkSpotManager.prototype.dispatchMeshActorJoin.call(
      manager,
      'mesh-a',
      { spotId: 'spot-a' },
      record
    )
  );
  try {
    await entered.promise;
    assert.equal(queue.snapshot().permitsInUse, 0n);
    const next = await queue.acquire();
    next.releaseAfterInternalProcessing();
  } finally {
    store.resolve({ actorType: 'Player' });
    await dispatch.catch(() => undefined);
    request.close();
  }
});

test('ActorBinding tombstone returns the ingress permit before remote binding retirement', async () => {
  const queue = queueWithOnePermit();
  const retired = deferred();
  const entered = deferred();
  const actor = { actorId: 'actor-a', nodeRid: 'node-a', generation: 1n };
  const binding = {
    kind: 'actorBinding',
    transition: 'tombstone',
    actor,
    actorNodeGeneration: 2n,
    authorityOwnerGeneration: 3n,
    sessionNodeRid: 'node-a',
    sessionOwnerNodeGeneration: 2n,
    sessionRid: 'session-a',
    bindingGeneration: 4n
  };
  const state = {
    nativeActorRef: actor,
    meshName: 'mesh-a',
    locationGeneration: 3n,
    isMoving: false,
    retireBoundSessionBinding() {}
  };
  const host = {
    actorManager: { getState: () => state },
    spotNodeRuntime: {
      meshNode: () => ({ status: () => ({ routingId: 'node-a', lifecycleGeneration: 2n }) })
    },
    streamBindingRuntime: {
      retireRemoteBinding: () => {
        entered.resolve();
        return retired.promise;
      }
    }
  };
  const record = {
    kind: ReceiveKind.ActorBinding,
    kindData: binding,
    reply: () => SubmitResult.Ok
  };
  const first = await queue.acquire();
  first.markApplicationQueued();
  const dispatch = runWithApplicationJobPermit(first, () =>
    ZLinkFrameworkRuntimeHost.prototype.dispatchMeshRecord.call(host, 'mesh-a', {}, record)
  );
  try {
    await entered.promise;
    assert.equal(queue.snapshot().permitsInUse, 0n);
    const next = await queue.acquire();
    next.releaseAfterInternalProcessing();
  } finally {
    retired.resolve(true);
    await dispatch;
  }
});
