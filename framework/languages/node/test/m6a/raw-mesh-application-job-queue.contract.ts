import assert from 'node:assert/strict';
import { test } from 'node:test';

import {
  ApplicationIngressRecordOwner
} from '../../packages/framework/src/runtime/application-jobs/application-ingress-record-owner';
import {
  ApplicationJobQueue,
  resolveApplicationJobQueueConfiguration
} from '../../packages/framework/src/runtime/host/application-job-queue';
import type {
  ZLinkRawBindingPort,
  ZLinkRawHostPort,
  ZLinkRawRouterPort
} from '../../packages/framework/src/runtime/backend/raw-binding-port';
import {
  RawServiceMeshRuntime
} from '../../packages/framework/src/runtime/foundation/raw-service-mesh-runtime';
import {
  ServiceMailbox
} from '../../packages/framework/src/runtime/foundation/service-mailbox';
import { SERVICE_WIRE_REQUIRED_CAPABILITY } from '../../packages/framework/src/runtime/foundation/service-wire-constants.generated';
import type {
  ServiceNodeDescriptor
} from '../../packages/framework/src/runtime/foundation/service-topology-registry';

function queue(limit = 1n): ApplicationJobQueue {
  return new ApplicationJobQueue(resolveApplicationJobQueueConfiguration(
    { maxQueuedApplicationJobs: limit },
    () => 1n
  ));
}

function descriptor(): ServiceNodeDescriptor {
  return {
    meshName: 'raw-job-queue',
    nodeRoutingId: 'raw-job-queue-node',
    lifecycleGeneration: 1n,
    descriptorRevision: 1n,
    advertisedEndpoint: 'inproc://raw-job-queue',
    channels: [],
    state: 'preparing',
    securityIdentity: 'default',
    applicationVersion: 1n,
    protocolCapabilities: [SERVICE_WIRE_REQUIRED_CAPABILITY],
    objectRole: 'server',
    placementWeight: 100,
    activeCapacityLimit: 1,
    pendingCapacityLimit: 1,
    activeCapacityUsed: 0,
    pendingCapacityUsed: 0
  };
}

test('raw receive waits for the host permit before touching the binding', async () => {
  const applicationJobs = queue();
  const occupied = await applicationJobs.acquire();
  let receives = 0;
  const router = {
    setRoutingId() {},
    bind() {},
    localEndpoint: () => 'inproc://raw-job-queue',
    monitor: () => ({ statusReady: () => false, close() {} }),
    receive: () => {
      receives += 1;
      return undefined;
    },
    close() {}
  } as unknown as ZLinkRawRouterPort;
  const host = {
    createRouter: () => router,
    close() {},
    shutdown() {}
  } as unknown as ZLinkRawHostPort;
  const binding = {
    createHost: () => host
  } satisfies ZLinkRawBindingPort;
  const runtime = new RawServiceMeshRuntime({
    descriptor: descriptor(),
    bindingPort: binding,
    applicationJobQueue: applicationJobs
  });
  runtime.start();
  try {
    const pumping = runtime.pumpOne();
    await new Promise(resolve => setImmediate(resolve));
    assert.equal(receives, 0);

    occupied.releaseAfterInternalProcessing();
    assert.equal(await pumping, 'noData');
    assert.equal(receives, 1);
    assert.equal(applicationJobs.snapshot().permitsInUse, 0n);
  } finally {
    runtime.close();
  }
});

test('raw shutdown cancels a pre-receive capacity wait without touching the binding', async () => {
  const applicationJobs = queue();
  const occupied = await applicationJobs.acquire();
  let receives = 0;
  const router = {
    setRoutingId() {},
    bind() {},
    localEndpoint: () => 'inproc://raw-job-queue-cancel',
    monitor: () => ({ statusReady: () => false, close() {} }),
    receive: () => {
      receives += 1;
      return undefined;
    },
    close() {}
  } as unknown as ZLinkRawRouterPort;
  const binding = {
    createHost: () => ({
      createRouter: () => router,
      close() {},
      shutdown() {}
    } as unknown as ZLinkRawHostPort)
  } satisfies ZLinkRawBindingPort;
  const runtime = new RawServiceMeshRuntime({
    descriptor: descriptor(),
    bindingPort: binding,
    applicationJobQueue: applicationJobs
  });
  runtime.start();

  const pumping = runtime.pumpOne();
  await new Promise(resolve => setImmediate(resolve));
  assert.equal(applicationJobs.snapshot().capacityWaiters, 1n);
  runtime.close();
  assert.equal(await pumping, 'noData');
  assert.equal(receives, 0);
  assert.equal(applicationJobs.snapshot().capacityWaiters, 0n);
  occupied.releaseAfterInternalProcessing();
  assert.equal(applicationJobs.snapshot().permitsInUse, 0n);
});

test('raw protocol drop closes its retained Core record and permit exactly once', async () => {
  const applicationJobs = queue();
  let retainedCloseCount = 0;
  const received = {
    sourceRid: 'malformed-peer',
    sourceRoute: Buffer.from('malformed-peer'),
    parts: [Buffer.from([0xff])],
    close() {
      retainedCloseCount += 1;
      assert.equal(retainedCloseCount, 1);
    }
  };
  let next = true;
  const router = {
    setRoutingId() {},
    bind() {},
    localEndpoint: () => 'inproc://raw-job-queue-protocol',
    monitor: () => ({ statusReady: () => false, close() {} }),
    receive: () => {
      if (!next) return undefined;
      next = false;
      return received;
    },
    close() {}
  } as unknown as ZLinkRawRouterPort;
  const binding = {
    createHost: () => ({
      createRouter: () => router,
      close() {},
      shutdown() {}
    } as unknown as ZLinkRawHostPort)
  } satisfies ZLinkRawBindingPort;
  const runtime = new RawServiceMeshRuntime({
    descriptor: descriptor(),
    bindingPort: binding,
    applicationJobQueue: applicationJobs
  });
  runtime.start();
  try {
    assert.equal(await runtime.pumpOne(), 'protocolError');
    assert.equal(retainedCloseCount, 1);
    assert.equal(applicationJobs.snapshot().permitsInUse, 0n);
  } finally {
    runtime.close();
  }
});

test('logical 1:N children acquire sequential permits and share one retained Core credit', async () => {
  const applicationJobs = queue();
  let retainedCloseCount = 0;
  const owner = ApplicationIngressRecordOwner.create(
    applicationJobs,
    await applicationJobs.acquire(),
    { close: () => retainedCloseCount += 1 }
  );
  let drained = 0;
  let resolveDrained!: () => void;
  const allDrained = new Promise<void>(resolve => resolveDrained = resolve);
  const mailbox = new ServiceMailbox(undefined, () => {
    setImmediate(() => {
      const claim = mailbox.tryClaim('application', 1, Number.MAX_SAFE_INTEGER);
      if (claim === undefined) return;
      const record = claim.records[0]!;
      record.applicationJob!.releaseBeforeHandler();
      record.applicationJob!.close();
      mailbox.release(claim);
      drained += 1;
      if (drained === 2) resolveDrained();
    });
  });

  for (const child of ['spot:first', 'spot:second']) {
    const applicationJob = await owner.acquire('application');
    assert.equal(mailbox.tryEnqueue({
      owner: child,
      domain: 'application',
      parts: [Buffer.from(child)],
      applicationJob
    }), true);
    assert.equal(applicationJobs.snapshot().permitsInUse, 1n);
  }
  owner.close();
  await allDrained;
  assert.equal(retainedCloseCount, 1);
  assert.equal(applicationJobs.snapshot().permitsInUse, 0n);
  assert.equal(applicationJobs.snapshot().peakPermitsInUse, 1n);
});

test('mailbox shutdown closes queued permits and retained credit without a leak', async () => {
  const applicationJobs = queue();
  let retainedCloseCount = 0;
  const owner = ApplicationIngressRecordOwner.create(
    applicationJobs,
    await applicationJobs.acquire(),
    { close: () => retainedCloseCount += 1 }
  );
  const applicationJob = await owner.acquire('application');
  const mailbox = new ServiceMailbox();
  assert.equal(mailbox.tryEnqueue({
    owner: 'node:raw-job-queue-node',
    domain: 'application',
    parts: [Buffer.from('retained')],
    applicationJob
  }), true);
  owner.close();

  mailbox.close();
  mailbox.close();
  assert.equal(retainedCloseCount, 1);
  assert.equal(applicationJobs.snapshot().permitsInUse, 0n);
});
