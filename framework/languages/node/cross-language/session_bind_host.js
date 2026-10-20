const readline = require('node:readline');
const {
  ZLinkNodeRawMeshBackend
} = require('../packages/framework/dist/runtime/backend/node/node-raw-mesh-backend');
const {
  ZLinkNodeRawBindingPort
} = require('../packages/framework/dist/runtime/backend/node/node-raw-binding-port');
const {
  ApplicationJobQueue,
  resolveApplicationJobQueueConfiguration
} = require('../packages/framework/dist/runtime/host/application-job-queue');
const {
  ReadyDomain
} = require('../packages/framework/dist/runtime/foundation/service-runtime-contracts');
const {
  encodeBoundSessionBindHeader,
  decodeStatefulReply
} = require('../packages/framework/dist/runtime/foundation/service-stateful-wire-codec');
const endpoint = process.argv[2];
const meshName = process.argv[3];
const ownerId = 'cross-bind-node-owner';
const backend = new ZLinkNodeRawMeshBackend(
  meshName,
  'cross-bind-node',
  new ZLinkNodeRawBindingPort(),
  new ApplicationJobQueue(
    resolveApplicationJobQueueConfiguration({ maxQueuedApplicationJobs: 2048n }, () => 1n)
  )
);
const peers = new Map();
let descriptorVisible = true;
backend.setBind(endpoint);
backend.setSessionOwnerDescriptors({
  local: () => ({
    nodeRid: 'cross-bind-node',
    nodeGeneration: backend.status().lifecycleGeneration,
    ownerId,
    leaseGeneration: 1n
  }),
  peer: async (rid, generation) => {
    const owner = peers.get(rid);
    return descriptorVisible && owner?.nodeGeneration === generation ? owner : undefined;
  }
});
backend.start();
let actor = backend.createActor('cross-bind-actor');
function output(value) {
  process.stdout.write(
    JSON.stringify(value, (_key, item) => (typeof item === 'bigint' ? item.toString() : item)) +
      '\n'
  );
}
// This fixture supplies the host's Actor binding control turn. ServiceStatefulRuntime
// retains validation, terminal encoding, binding publication and retirement.
const pump = setInterval(() => {
  const ready = backend.createReadyBatch(8);
  try {
    const batch = backend.drainReady(ReadyDomain.Application | ReadyDomain.Infrastructure, ready);
    for (let i = 0; i < batch.records.length; i++) {
      const claim = ready.takeClaim(i);
      const received = backend.createReceiveBatch(16, 16);
      try {
        const result = claim.recvBatch(received);
        for (const record of result.records) {
          if (record.kindData?.kind === 'actorBinding') record.reply(Buffer.alloc(0));
          for (const part of record.parts) part.close();
        }
      } finally {
        received.close();
        claim.release();
      }
    }
  } finally {
    ready.close();
  }
}, 1);
output({
  kind: 'ready',
  actor,
  endpoint: backend.status().localEndpoint,
  generation: backend.status().lifecycleGeneration,
  authority: backend.stateful.registry.actor(actor.actorId).authorityOwnerGeneration,
  ownerId
});
async function command(value) {
  if (value.kind === 'actor') {
    actor = backend.restoreActorAuthority(
      actor.actorId,
      'actor',
      BigInt(value.generation),
      BigInt(value.authority),
      undefined,
      1n,
      1n
    );
    return { kind: 'actor' };
  }
  if (value.kind === 'configure') {
    peers.set(value.rid, {
      nodeRid: value.rid,
      nodeGeneration: BigInt(value.generation),
      ownerId: value.ownerId,
      leaseGeneration: BigInt(value.lease)
    });
    await backend.connectPeer({
      endpoint: value.endpoint,
      expectedRid: value.rid,
      expectedLifecycleGeneration: BigInt(value.generation)
    });
    return { kind: 'configured' };
  }
  if (value.kind === 'descriptor') {
    descriptorVisible = value.visible;
    return { kind: 'descriptor', visible: descriptorVisible };
  }
  if (value.kind === 'binding') {
    return { kind: 'binding', binding: backend.stateful.registry.binding(actor) ?? null };
  }
  const target = {
    nodeRid: value.rid,
    actorId: value.actorId,
    generation: BigInt(value.objectGeneration)
  };
  const authority = {
    targetNodeGeneration: BigInt(value.nodeGeneration),
    authorityOwnerGeneration: BigInt(value.authority),
    ownerLeaseGeneration: BigInt(value.lease)
  };
  if (value.kind === 'bind') {
    const pending = backend.stateful.bindSession(
      value.session,
      target,
      2000,
      () => {},
      undefined,
      authority
    );
    return { kind: 'terminal', result: await pending.promise };
  }
  if (value.kind === 'tombstone' || value.kind === 'register') {
    const correlation = 900n;
    const header = encodeBoundSessionBindHeader(
      correlation,
      { actor: target, ...authority },
      value.session,
      value.kind === 'register'
        ? { state: 'active', generation: BigInt(value.bindingGeneration) }
        : { state: 'tombstone', retiredGeneration: BigInt(value.bindingGeneration) }
    );
    const reply = await backend.runtime.requestService(value.rid, [header], 2000);
    return {
      kind: 'terminal',
      result: decodeStatefulReply(
        reply[0],
        correlation,
        value.kind === 'register' ? 'streamBind' : 'streamUnbind'
      )
    };
  }
  throw new Error(`Unsupported fixture command: ${value.kind}`);
}
let chain = Promise.resolve();
readline
  .createInterface({ input: process.stdin })
  .on('line', (line) => {
    chain = chain.then(async () => {
      try {
        output(await command(JSON.parse(line)));
      } catch (error) {
        output({ kind: 'error', name: error.name, message: error.message, errorKind: error.kind });
      }
    });
  })
  .on('close', async () => {
    await chain;
    clearInterval(pump);
    backend.close();
  });
