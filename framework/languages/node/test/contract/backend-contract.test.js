const assert = require('node:assert/strict');
const { once } = require('node:events');
const { spawn } = require('node:child_process');
const net = require('node:net');
const path = require('node:path');
const nodeTest = require('node:test');

const zlink = require('@zlink-systems/zlink');
const backend = require('../../packages/framework/dist/runtime/backend');
const framework = require('../../packages/framework/dist/internal');
const {
  ZLinkSubmitStatus
} = require('../../packages/framework/dist/runtime/messaging/submission-result');
const channelEnvelope = require('../../packages/framework/dist/runtime/channels/channel-envelope');
const {
  isPollerInterruptedError,
  submitBindingSyncSend
} = require('../../packages/framework/dist/runtime/backend/node/node-backend-adapter-support');
const { wrapSocket } = require('../../packages/framework/dist/runtime/backend/node/node-socket-backend-adapter');
const {
  ZLinkMeshCompletionTable,
  closeMeshCompletion
} = require('../../packages/framework/dist/runtime/backend/mesh-completion-table');
const {
  ApplicationJobQueue,
  resolveApplicationJobQueueConfiguration
} = require('../../packages/framework/dist/runtime/host/application-job-queue');
const {
  releaseApplicationJobPermitBeforeHandler
} = require('../../packages/framework/dist/runtime/application-jobs/application-job-queue-scope');
const {
  ZLinkSpotSerialTurnExecutor
} = require('../../packages/framework/dist/runtime/spots');

function applicationJobQueue(maxQueuedApplicationJobs) {
  return new ApplicationJobQueue(resolveApplicationJobQueueConfiguration(
    maxQueuedApplicationJobs === undefined ? {} : { maxQueuedApplicationJobs }
  ));
}

function wrapSpotSendSocket(submit) {
  return wrapSocket({
    close() {},
    request() {},
    reply() {},
    sendToSpot() {
      return {
        message() {
          return submit;
        }
      };
    }
  });
}

const removedSpotAdapterContracts = new Set([
  'backend adapter unwraps SpotNode when attaching stream SessionRelay',
  'backend stream bind converts public string actor node RID to native RoutingId',
  'backend adapter converts public string route bridge target RIDs to native RoutingId',
  'channel runtime connects route mesh peers before bind and spot route bridge attach',
  'backend adapter normalizes missing SpotNode actor lookup to undefined',
  'backend adapter submits actor destroy and closes actor bound sessions through public binding operations',
  'backend adapter aborts actor destroy waits and closes late reply messages',
  'backend adapter submits SpotNode actor bound-session send operation',
  'backend adapter submits SpotNode actor send through async completion callback',
  'backend adapter submits SpotNode actor request builder and wires callback'
]);

function test(name, body) {
  if (!removedSpotAdapterContracts.has(name)) {
    return nodeTest(name, body);
  }
  return nodeTest(`${name} is superseded by the formal MeshNode backend`, () => {
    const factory = new backend.ZLinkNodeBackendAdapterFactory();
    assert.equal(typeof factory.createMeshAdapter, 'function');
    assert.equal(factory.createSpotAdapter, undefined);
  });
}

async function submitEventually(submit, timeoutMs = 2000) {
  const deadline = Date.now() + timeoutMs;
  let result;
  do {
    result = submit();
    if (result === zlink.SubmitResult.Ok) return result;
    await new Promise((resolve) => setTimeout(resolve, 5));
  } while (Date.now() < deadline);
  return result;
}

async function waitEventually(predicate, timeoutMs = 2000) {
  const deadline = Date.now() + timeoutMs;
  do {
    if (predicate()) return;
    await new Promise((resolve) => setTimeout(resolve, 5));
  } while (Date.now() < deadline);
  throw new Error('Condition did not become true before the timeout.');
}

async function reserveTcpEndpoint() {
  const server = net.createServer();
  await new Promise((resolve, reject) => {
    server.once('error', reject);
    server.listen(0, '127.0.0.1', resolve);
  });
  const address = server.address();
  await new Promise((resolve, reject) => server.close((error) => error ? reject(error) : resolve()));
  return `tcp://127.0.0.1:${address.port}`;
}

test('backend adapter factory exposes the supported backend adapters', () => {
  const factory = new backend.ZLinkNodeBackendAdapterFactory();

  assert.equal(typeof factory.createChannelAdapter, 'function');
  assert.equal(typeof factory.createMeshAdapter, 'function');
  assert.equal(factory.createSpotAdapter, undefined);
  assert.equal(typeof factory.createStreamAdapter, 'function');
  assert.equal(typeof factory.createMonitoringAdapter, 'function');
  assert.equal(factory.createRegistryAdapter, undefined);
});

test('poller interruption is treated as an empty progress turn', () => {
  assert.equal(
    isPollerInterruptedError(new zlink.RecvError(zlink.RecvResult.NoData, 4)),
    true
  );
  assert.equal(
    isPollerInterruptedError(new zlink.RecvError(zlink.RecvResult.NoData, 11)),
    false
  );
});

test('binding synchronous send uses the flag-free finalized terminal', () => {
  const submittedParts = [];
  const submit = {
    message(part) {
      submittedParts.push(Buffer.from(part));
      return this;
    },
    submit_sync() {
      return undefined;
    }
  };
  const operation = {
    message(part) {
      submittedParts.push(Buffer.from(part));
      return submit;
    }
  };

  assert.equal(submitBindingSyncSend(operation, Buffer.from('blocking')), undefined);
  assert.equal(submitBindingSyncSend(operation, Buffer.from('nonblocking')), undefined);
  assert.deepEqual(submittedParts.map((part) => part.toString()), ['blocking', 'nonblocking']);
});

test('backend DONTWAIT Spot send awaits managed binding admission', async () => {
  let releaseAdmission;
  const admission = new Promise((resolve) => {
    releaseAdmission = resolve;
  });
  let asyncCalls = 0;
  let syncCalls = 0;
  const submit = {
    message() {
      return this;
    },
    submit() {
      asyncCalls += 1;
      return admission;
    },
    submit_sync() {
      syncCalls += 1;
    }
  };
  const socket = wrapSpotSendSocket(submit);

  let settled = false;
  const pending = socket.sendToSpot(
    'target-node',
    'target-spot',
    [Buffer.from('payload')],
    zlink.SendFlags.DontWait
  ).then(() => {
    settled = true;
  });
  await Promise.resolve();

  assert.equal(asyncCalls, 1);
  assert.equal(syncCalls, 0);
  assert.equal(settled, false);

  releaseAdmission();
  await pending;
  assert.equal(settled, true);
});

test('backend managed DONTWAIT Spot send surfaces terminal binding failure', async () => {
  const terminal = new zlink.SubmitError(zlink.SubmitResult.NotFound, 2);
  const submit = {
    message() {
      return this;
    },
    submit() {
      return Promise.reject(terminal);
    },
    submit_sync() {
      throw new Error('managed DONTWAIT send must not use submit_sync');
    }
  };
  const socket = wrapSpotSendSocket(submit);

  await assert.rejects(
    socket.sendToSpot(
      'target-node',
      'target-spot',
      [Buffer.from('payload')],
      zlink.SendFlags.DontWait
    ),
    (error) => error instanceof backend.ZLinkBackendResultError
      && error.operation === 'submit'
      && error.result === zlink.SubmitResult.NotFound
      && error.nativeErrno === 2
      && error.cause === terminal
  );
});

test('RouteMesh runtime weight changes reject invalid values as configuration errors', () => {
  const registration = framework.createFrameworkRegistrationWithBuilder((builder) => {
    builder.addRouteMesh('game')
      .channel('events')
      .server()
      .setWeight(100);
  });
  const calls = [];
  const manager = Object.create(framework.ZLinkSpotNodeRuntimeManager.prototype);
  manager.options = { registration };
  manager.meshNodes = new Map([['game', {
    setPlacementWeight() { calls.push('placement'); },
    setChannelWeight() { calls.push('channel'); }
  }]]);

  for (const invalid of [-1, 10_001, 1.5, Number.NaN]) {
    assert.throws(
      () => manager.setRuntimePlacementWeight('game', invalid),
      (error) => error instanceof framework.ZLinkConfigurationException
    );
    assert.throws(
      () => manager.setRuntimeChannelWeight('events', invalid),
      (error) => error instanceof framework.ZLinkConfigurationException
    );
  }
  assert.deepEqual(calls, []);
});

test('MeshNode descriptors keep an explicit Spot stable type instead of adding a class-name alias', async () => {
  class ConfiguredRoomSpot {}
  const registration = framework.createFrameworkRegistrationWithBuilder((builder) => {
    builder.addRouteMesh('game')
      .objects().server()
      .addSpotFactory('game.room', ConfiguredRoomSpot, (factory) => factory.disableRelocation());
  });
  const manager = new framework.ZLinkSpotNodeRuntimeManager({ registration });
  manager.meshNodes.set('game', {
    status: () => ({
      routingId: 'game-node',
      lifecycleGeneration: 1n,
      localEndpoint: 'tcp://127.0.0.1:9400'
    })
  });
  let published;
  manager.locationAutoConnect = {
    runtime: {
      currentOwnerToken: { ownerId: 'owner-a', leaseGeneration: 1n },
      writeMeshNode: async (descriptor) => {
        published = descriptor;
        return {
          status: framework.ZLinkLocationWriteStatus.Stored,
          updatedAt: new Date()
        };
      }
    }
  };

  await manager.publishMeshNodeState(framework.ZLinkFrameworkRuntimeState.Serving);

  assert.deepEqual(
    published.objectCapabilities
      .filter((capability) => capability.objectKind === 'user_spot')
      .map((capability) => capability.stableType),
    ['game.room']
  );
  assert.deepEqual(published.spotTypes, ['game.room']);
});

test('backend raw STREAM does not expose the removed bindings Session Actor service API', async () => {
  const factory = new backend.ZLinkNodeBackendAdapterFactory();
  const channel = factory.createChannelAdapter();
  const streamAdapter = factory.createStreamAdapter();
  const context = channel.createContext();
  const stream = streamAdapter.createStreamSocket(context);

  try {
    assert.equal(typeof stream.nativeInstance.bindActor, 'undefined');
    assert.equal(typeof stream.nativeInstance.unbindActor, 'undefined');
    assert.equal(typeof stream.nativeInstance.sendBoundActor, 'undefined');
    await assert.rejects(
      () => stream.bindActor(
        'session-rid',
        { nodeRid: 'node-rid', actorId: 'actor-id', generation: 1n },
        1000
      ),
      /requires enableActorDispatch\(\) and a Framework MeshNode service route/
    );
  } finally {
    await stream.dispose();
    await context.dispose();
  }
});

test('backend mesh adapter creates a named MeshNode through the public binding API', async () => {
  const factory = new backend.ZLinkNodeBackendAdapterFactory();
  const context = factory.createChannelAdapter().createContext();
  const meshNode = factory.createMeshAdapter().createMeshNode(context, {
    meshName: 'backend.contract',
    routingId: `backend-node-${process.pid}`,
    applicationJobQueue: applicationJobQueue()
  });

  try {
    meshNode.setBind(`inproc://backend-contract-${process.pid}`);
    meshNode.addChannelName('backend.channel');
    meshNode.start();
    assert.equal(meshNode.status().meshName, 'backend.contract');
    assert.equal(meshNode.status().channelCount, 1);
    assert.equal(meshNode.shutdown(1000), zlink.RequestResult.Ok);
  } finally {
    meshNode.close();
    await context.dispose();
  }
});

test('new MeshNode processes receive distinct lifecycle generations', async () => {
  const factory = new backend.ZLinkNodeBackendAdapterFactory();
  const context = factory.createChannelAdapter().createContext();
  const meshAdapter = factory.createMeshAdapter();
  const first = meshAdapter.createMeshNode(context, {
    meshName: 'backend.generation',
    routingId: 'backend-generation-node',
    applicationJobQueue: applicationJobQueue()
  });
  const second = meshAdapter.createMeshNode(context, {
    meshName: 'backend.generation',
    routingId: 'backend-generation-node',
    applicationJobQueue: applicationJobQueue()
  });

  try {
    const firstGeneration = first.status().lifecycleGeneration;
    const secondGeneration = second.status().lifecycleGeneration;
    assert.ok(firstGeneration > 0n);
    assert.ok(secondGeneration > 0n);
    assert.ok(firstGeneration <= BigInt(Number.MAX_SAFE_INTEGER));
    assert.ok(secondGeneration <= BigInt(Number.MAX_SAFE_INTEGER));
    assert.notEqual(firstGeneration, secondGeneration);
  } finally {
    first.close();
    second.close();
    await context.dispose();
  }
});

test('backend mesh dispatch pump drains a local channel record through claim and receive batches', async () => {
  const factory = new backend.ZLinkNodeBackendAdapterFactory();
  const context = factory.createChannelAdapter().createContext();
  const meshName = `backend.dispatch.${process.pid}`;
  const receiver = factory.createMeshAdapter().createMeshNode(context, {
    meshName,
    routingId: `backend-dispatch-receiver-${process.pid}`,
    applicationJobQueue: applicationJobQueue()
  });
  const sender = factory.createMeshAdapter().createMeshNode(context, {
    meshName,
    routingId: `backend-dispatch-sender-${process.pid}`,
    applicationJobQueue: applicationJobQueue()
  });
  let pump;
  let senderPump;

  try {
    const receiverEndpoint = await reserveTcpEndpoint();
    const senderEndpoint = await reserveTcpEndpoint();
    receiver.setBind(receiverEndpoint);
    receiver.addChannelName('backend.dispatch');
    receiver.start();
    sender.setBind(senderEndpoint);
    sender.start();
    sender.connectPeer({
      endpoint: receiverEndpoint,
      expectedRid: receiver.status().routingId,
      expectedSecurityIdentity: 'default'
    });
    const received = new Promise((resolve, reject) => {
      const timeout = setTimeout(
        () => reject(new Error('Mesh dispatch pump did not receive the local channel record.')),
        5000
      );
      pump = new backend.ZLinkMeshDispatchPump(receiver, {
        applicationJobQueue: applicationJobQueue(),
        dispatch(_owner, record) {
          clearTimeout(timeout);
          resolve({
            kind: record.kind,
            channelName: record.channelName,
            payload: record.parts[0].data().toString()
          });
        },
        reportError(error) {
          clearTimeout(timeout);
          reject(error);
        }
      });
      pump.start();
    });
    senderPump = new backend.ZLinkMeshDispatchPump(sender, {
      applicationJobQueue: applicationJobQueue(),
      dispatch() {},
      reportError(error) { throw error; }
    });
    senderPump.start();

    assert.equal(await submitEventually(
      () => sender.sendToChannel('backend.dispatch', Buffer.from('mesh-pump'))
    ), zlink.SubmitResult.Ok);
    const record = await received;
    assert.equal(record.kind, framework.ReceiveKind.ChannelSend);
    assert.equal(record.channelName, 'backend.dispatch');
    assert.equal(record.payload, 'mesh-pump');
    await waitEventually(() => sender.peers().find(peer =>
      String(peer.routingId) === String(receiver.status().routingId))?.state === 3
      && receiver.peers().find(peer =>
        String(peer.routingId) === String(sender.status().routingId))?.state === 3);
  } finally {
    await senderPump?.dispose();
    await pump?.dispose();
    sender.shutdown(1000);
    sender.close();
    receiver.shutdown(1000);
    receiver.close();
    await context.dispose();
  }
});

test('backend mesh dispatch pump requeues records beyond the receive batch capacity', async () => {
  const factory = new backend.ZLinkNodeBackendAdapterFactory();
  const context = factory.createChannelAdapter().createContext();
  const meshName = `backend.dispatch.sequence.${process.pid}`;
  const node = factory.createMeshAdapter().createMeshNode(context, {
    meshName,
    routingId: `backend-dispatch-sequence-${process.pid}`,
    applicationJobQueue: applicationJobQueue()
  });
  const received = [];
  let resolveReceived;
  let rejectReceived;
  const completed = new Promise((resolve, reject) => {
    resolveReceived = resolve;
    rejectReceived = reject;
  });
  let pump;

  try {
    node.setBind(`inproc://backend-dispatch-sequence-${process.pid}`);
    node.addChannelName('backend.sequence');
    node.start();
    pump = new backend.ZLinkMeshDispatchPump(node, {
      applicationJobQueue: applicationJobQueue(),
      dispatch(_owner, record) {
        received.push(record.parts[0].data().toString());
        if (received.length === 2) resolveReceived();
      },
      reportError(error) {
        rejectReceived(error);
      }
    });
    pump.start();

    assert.equal(await node.sendToChannel('backend.sequence', Buffer.from('first')), zlink.SubmitResult.Ok);
    assert.equal(await node.sendToChannel('backend.sequence', Buffer.from('second')), zlink.SubmitResult.Ok);
    await Promise.race([
      completed,
      new Promise((_, reject) => setTimeout(() => reject(new Error('Sequential mesh records were not both dispatched.')), 2000))
    ]);
    assert.deepEqual(received, ['first', 'second']);
  } finally {
    await pump?.dispose();
    node.shutdown(1000);
    node.close();
    await context.dispose();
  }
});

test('backend mesh terminal completion bypasses application job queue capacity', async () => {
  let readyHandler;
  let received = false;
  let completed;
  const completion = new Promise((resolve) => { completed = resolve; });
  const claim = {
    recvBatch() {
      if (received) return { ok: false, records: [] };
      received = true;
      return { ok: true, records: [{ parts: [] }] };
    },
    release() {}
  };
  const node = {
    setReadyHandler(handler) { readyHandler = handler; },
    createReadyBatch() {
      return { reset() {}, takeClaim() { return claim; }, close() {} };
    },
    createReceiveBatch() {
      return { reset() {}, close() {} };
    },
    drainReady() {
      return {
        ok: true,
        hasResidue: false,
        records: [{
          ownerKind: framework.ReadyOwnerKind.Node,
          terminalCompletion: true
        }]
      };
    }
  };
  const pump = new backend.ZLinkMeshDispatchPump(node, {
    applicationJobQueue: {
      acquire() {
        throw new Error('terminal completion must not acquire application capacity');
      }
    },
    dispatch() { completed(); }
  });

  try {
    pump.start();
    readyHandler(framework.ReadyDomain.Infrastructure);
    await completion;
  } finally {
    await pump.dispose();
  }
});

test('backend mesh dispatch releases a reserved permit when a claim batch is empty', async () => {
  let readyHandler;
  let receiveCount = 0;
  const queue = applicationJobQueue(1n);
  const claim = {
    recvBatch() {
      receiveCount += 1;
      return receiveCount === 1
        ? { ok: true, records: [] }
        : { ok: false, records: [] };
    },
    release() {}
  };
  const node = {
    setReadyHandler(handler) { readyHandler = handler; },
    createReadyBatch() {
      return { reset() {}, takeClaim() { return claim; }, close() {} };
    },
    createReceiveBatch() {
      return { reset() {}, close() {} };
    },
    drainReady() {
      return {
        ok: true,
        hasResidue: false,
        records: [{ ownerKind: framework.ReadyOwnerKind.Node }]
      };
    }
  };
  const pump = new backend.ZLinkMeshDispatchPump(node, {
    applicationJobQueue: queue,
    dispatch() { throw new Error('empty claim must not dispatch'); }
  });

  try {
    pump.start();
    readyHandler(framework.ReadyDomain.Infrastructure);
    await waitEventually(() => receiveCount >= 2);
    assert.equal(queue.snapshot().permitsInUse, 0n);
  } finally {
    await pump.dispose();
  }
});

test('backend mesh dispatch reports service-wire command context before closing a failed record', async () => {
  let readyHandler;
  let received = false;
  let partClosed = false;
  let resolveFailure;
  const failure = new Promise((resolve) => { resolveFailure = resolve; });
  const part = {
    data() { return Buffer.from([0x5a, 0x4d, 0x01, 34, 0x00]); },
    close() { partClosed = true; }
  };
  const claim = {
    recvBatch() {
      if (received) return { ok: false, records: [] };
      received = true;
      return {
        ok: true,
        records: [{
          kind: framework.ReceiveKind.ChannelSend,
          operationKind: 0,
          packetName: undefined,
          sourceNodeRid: 'source-node',
          parts: [part]
        }]
      };
    },
    release() {}
  };
  const node = {
    setReadyHandler(handler) { readyHandler = handler; },
    createReadyBatch() {
      return { reset() {}, takeClaim() { return claim; }, close() {} };
    },
    createReceiveBatch() {
      return { reset() {}, close() {} };
    },
    drainReady() {
      return {
        ok: true,
        hasResidue: false,
        records: [{ ownerKind: framework.ReadyOwnerKind.Node }]
      };
    }
  };
  const rootCause = new Error('target authority lost relocation envelope');
  const pump = new backend.ZLinkMeshDispatchPump(node, {
    applicationJobQueue: applicationJobQueue(),
    dispatch() { throw rootCause; },
    reportError(error, context) { resolveFailure({ error, context }); }
  });

  try {
    pump.start();
    readyHandler(framework.ReadyDomain.Infrastructure);
    const reported = await failure;
    assert.equal(reported.error, rootCause);
    assert.equal(reported.context.commandId, 34);
    assert.equal(reported.context.packetName, undefined);
    assert.equal(reported.context.sourceNodeRid, 'source-node');
    assert.equal(partClosed, true);
  } finally {
    await pump.dispose();
  }
});

test('backend mesh dispatch pump yields between continuous receive batches', async () => {
  const totalBatches = 32;
  let readyHandler;
  let receivedBatches = 0;
  let timerObservedAt;
  let complete;
  const completed = new Promise((resolve) => { complete = resolve; });
  const claim = {
    recvBatch() {
      if (receivedBatches >= totalBatches) return { ok: false, records: [] };
      receivedBatches += 1;
      return { ok: true, records: [{ parts: [] }] };
    },
    release() {}
  };
  const node = {
    setReadyHandler(handler) { readyHandler = handler; },
    createReadyBatch() {
      return {
        reset() {},
        takeClaim() { return claim; },
        close() {}
      };
    },
    createReceiveBatch() {
      return {
        reset() {},
        close() {}
      };
    },
    drainReady() {
      return {
        ok: true,
        hasResidue: false,
        records: [{ ownerKind: framework.ReadyOwnerKind.Node }]
      };
    }
  };
  const pump = new backend.ZLinkMeshDispatchPump(node, {
    applicationJobQueue: applicationJobQueue(),
    dispatch() {
      if (receivedBatches === totalBatches) complete();
    }
  });

  try {
    pump.start();
    setTimeout(() => { timerObservedAt = receivedBatches; }, 0);
    readyHandler(framework.ReadyDomain.Application);
    await completed;
    assert.ok(timerObservedAt < totalBatches, `timer ran after ${timerObservedAt} batches`);
  } finally {
    await pump.dispose();
  }
});

test('backend mesh dispatch pump yields when elapsed receive time wins before the count limit', async () => {
  const totalBatches = 4;
  let readyHandler;
  let receivedBatches = 0;
  let timerObservedAt;
  let nowMs = 0;
  let complete;
  const completed = new Promise((resolve) => { complete = resolve; });
  const claim = {
    recvBatch() {
      if (receivedBatches >= totalBatches) return { ok: false, records: [] };
      receivedBatches += 1;
      return { ok: true, records: [{ parts: [] }] };
    },
    release() {}
  };
  const node = {
    setReadyHandler(handler) { readyHandler = handler; },
    createReadyBatch() {
      return {
        reset() {},
        takeClaim() { return claim; },
        close() {}
      };
    },
    createReceiveBatch() {
      return { reset() {}, close() {} };
    },
    drainReady() {
      return {
        ok: true,
        hasResidue: false,
        records: [{ ownerKind: framework.ReadyOwnerKind.Node }]
      };
    }
  };
  const pump = new backend.ZLinkMeshDispatchPump(node, {
    applicationJobQueue: applicationJobQueue(),
    monotonicNowMs: () => {
      nowMs += 3;
      return nowMs;
    },
    dispatch() {
      if (receivedBatches === totalBatches) complete();
    }
  });

  try {
    pump.start();
    setTimeout(() => { timerObservedAt = receivedBatches; }, 0);
    readyHandler(framework.ReadyDomain.Application);
    await completed;
    assert.ok(timerObservedAt < totalBatches, `timer ran after ${timerObservedAt} batches`);
  } finally {
    await pump.dispose();
  }
});

test('backend mesh dispatch pump marks application and infrastructure execution areas', async () => {
  let readyHandler;
  let currentDomain;
  const pending = new Set([
    framework.ReadyDomain.Infrastructure,
    framework.ReadyDomain.Application
  ]);
  const observed = [];
  let complete;
  const completed = new Promise((resolve) => { complete = resolve; });
  const node = {
    setReadyHandler(handler) { readyHandler = handler; },
    createReadyBatch() {
      return {
        reset() {},
        takeClaim() {
          let consumed = false;
          const domain = currentDomain;
          return {
            recvBatch() {
              if (consumed) return { ok: false, records: [] };
              consumed = true;
              return { ok: true, records: [{ domain, parts: [] }] };
            },
            release() {}
          };
        },
        close() {}
      };
    },
    createReceiveBatch() {
      return { reset() {}, close() {} };
    },
    drainReady(domain) {
      currentDomain = domain;
      if (!pending.delete(domain)) return { ok: false, hasResidue: false, records: [] };
      return {
        ok: true,
        hasResidue: false,
        records: [{ ownerKind: framework.ReadyOwnerKind.Node, domain }]
      };
    }
  };
  const pump = new backend.ZLinkMeshDispatchPump(node, {
    applicationJobQueue: applicationJobQueue(),
    dispatch(_owner, record) {
      observed.push([record.domain, framework.currentZLinkExecutionArea()]);
      if (observed.length === 2) complete();
    }
  });

  try {
    pump.start();
    readyHandler(framework.ReadyDomain.Infrastructure | framework.ReadyDomain.Application);
    await completed;
    assert.deepEqual(observed, [
      [framework.ReadyDomain.Infrastructure, 'infrastructure'],
      [framework.ReadyDomain.Application, 'application']
    ]);
  } finally {
    await pump.dispose();
  }
});

test('backend mesh dispatch pump yields between continuous ready batches without messages', async () => {
  const totalBatches = 32;
  let readyHandler;
  let drainedBatches = 0;
  let timerObservedAt;
  let complete;
  const completed = new Promise((resolve) => { complete = resolve; });
  const node = {
    setReadyHandler(handler) { readyHandler = handler; },
    createReadyBatch() {
      return {
        reset() {},
        takeClaim() {
          return {
            recvBatch() { return { ok: false, records: [] }; },
            release() {}
          };
        },
        close() {}
      };
    },
    createReceiveBatch() {
      return {
        reset() {},
        close() {}
      };
    },
    drainReady() {
      drainedBatches += 1;
      if (drainedBatches === totalBatches) complete();
      return {
        ok: true,
        hasResidue: drainedBatches < totalBatches,
        records: [{ ownerKind: framework.ReadyOwnerKind.Node }]
      };
    }
  };
  const pump = new backend.ZLinkMeshDispatchPump(node, {
    applicationJobQueue: applicationJobQueue(),
    dispatch() {}
  });

  try {
    pump.start();
    setTimeout(() => { timerObservedAt = drainedBatches; }, 0);
    readyHandler(framework.ReadyDomain.Application);
    await completed;
    assert.ok(timerObservedAt < totalBatches, `timer ran after ${timerObservedAt} ready batches`);
  } finally {
    await pump.dispose();
  }
});

test('backend mesh dispatch pump bounds lifecycle priority so application work is not starved', async () => {
  let readyHandler;
  const lifecycle = Array.from({ length: 8 }, (_, index) => `lifecycle-${index}`);
  const application = ['application-0'];
  const order = [];

  function createClaim(label) {
    let consumed = false;
    return {
      recvBatch() {
        if (consumed) return { ok: false, records: [] };
        consumed = true;
        return {
          ok: true,
          records: [{
            parts: [zlink.Message.from(label)],
            operationKind: 0
          }]
        };
      },
      release() {}
    };
  }

  const node = {
    setReadyHandler(handler) { readyHandler = handler; },
    createReadyBatch(capacity) {
      const batch = {
        records: [],
        claims: [],
        get full() { return this.records.length >= capacity; },
        push(record, claim) {
          this.records.push(record);
          this.claims.push(claim);
        },
        reset() {
          this.records.length = 0;
          this.claims.length = 0;
        },
        takeClaim(index) { return this.claims[index]; },
        close() {}
      };
      return batch;
    },
    createReceiveBatch() {
      return { reset() {}, close() {} };
    },
    drainReady(domain, batch) {
      const queue = domain === framework.ReadyDomain.Infrastructure ? lifecycle : application;
      while (queue.length > 0 && !batch.full) {
        const label = queue.shift();
        batch.push(
          { ownerKind: framework.ReadyOwnerKind.Node, domain },
          createClaim(label)
        );
      }
      return {
        ok: true,
        hasResidue: (domain === framework.ReadyDomain.Infrastructure
          ? lifecycle
          : application).length > 0,
        records: batch.records
      };
    }
  };
  const pump = new backend.ZLinkMeshDispatchPump(node, {
    applicationJobQueue: applicationJobQueue(),
    dispatch(_owner, record) {
      order.push(record.parts[0].getString('utf8'));
    }
  });

  try {
    pump.start();
    readyHandler(framework.ReadyDomain.Infrastructure | framework.ReadyDomain.Application);
    for (let turn = 0; turn < 100 && order.length < 9; turn += 1) {
      await new Promise((resolve) => setTimeout(resolve, 0));
    }
    await pump.dispose();
  } finally {
    await pump.dispose();
  }

  assert.deepEqual(order.slice(0, 5), [
    'lifecycle-0',
    'lifecycle-1',
    'lifecycle-2',
    'lifecycle-3',
    'application-0'
  ]);
  assert.equal(order.length, 9);
});

test('backend mesh dispatch pump yields before recursively signaled dispatch work', async () => {
  const totalDispatches = 64;
  let readyHandler;
  let dispatches = 0;
  let timerObservedAt;
  let complete;
  const completed = new Promise((resolve) => { complete = resolve; });
  const node = {
    setReadyHandler(handler) { readyHandler = handler; },
    createReadyBatch() {
      return {
        reset() {},
        takeClaim() {
          let received = false;
          return {
            recvBatch() {
              if (received) return { ok: false, records: [] };
              received = true;
              return { ok: true, records: [{ parts: [] }] };
            },
            release() {}
          };
        },
        close() {}
      };
    },
    createReceiveBatch() {
      return {
        reset() {},
        close() {}
      };
    },
    drainReady() {
      return {
        ok: true,
        hasResidue: false,
        records: [{ ownerKind: framework.ReadyOwnerKind.Node }]
      };
    }
  };
  const pump = new backend.ZLinkMeshDispatchPump(node, {
    applicationJobQueue: applicationJobQueue(),
    dispatch() {
      dispatches += 1;
      if (dispatches === totalDispatches) {
        complete();
      } else {
        readyHandler(framework.ReadyDomain.Application);
      }
    }
  });

  try {
    pump.start();
    setTimeout(() => { timerObservedAt = dispatches; }, 0);
    readyHandler(framework.ReadyDomain.Application);
    await completed;
    assert.ok(timerObservedAt < totalDispatches, `timer ran after ${timerObservedAt} recursive dispatches`);
  } finally {
    await pump.dispose();
  }
});

test('backend mesh dispatch pump drains ready work queued before an async handler waits', async () => {
  let readyHandler;
  let drainedBatches = 0;
  let resolveFirstDispatch;
  const firstDispatchReleased = new Promise((resolve) => { resolveFirstDispatch = resolve; });
  let resolveSecondDispatch;
  const secondDispatched = new Promise((resolve) => { resolveSecondDispatch = resolve; });
  const node = {
    setReadyHandler(handler) { readyHandler = handler; },
    createReadyBatch() {
      return {
        reset() {},
        takeClaim() {
          let received = false;
          const batch = drainedBatches;
          return {
            recvBatch() {
              if (received) return { ok: false, records: [] };
              received = true;
              return { ok: true, records: [{ parts: [], batch }] };
            },
            release() {}
          };
        },
        close() {}
      };
    },
    createReceiveBatch() {
      return {
        reset() {},
        close() {}
      };
    },
    drainReady() {
      drainedBatches += 1;
      if (drainedBatches === 1) {
        readyHandler(framework.ReadyDomain.Application);
      }
      return {
        ok: true,
        hasResidue: false,
        records: [{ ownerKind: framework.ReadyOwnerKind.Node }]
      };
    }
  };
  const pump = new backend.ZLinkMeshDispatchPump(node, {
    applicationJobQueue: applicationJobQueue(),
    async dispatch(_owner, record) {
      if (record.batch === 1) {
        await firstDispatchReleased;
        return;
      }
      resolveSecondDispatch();
      resolveFirstDispatch();
    }
  });

  try {
    pump.start();
    readyHandler(framework.ReadyDomain.Application);
    await Promise.race([
      secondDispatched,
      new Promise((_, reject) => setTimeout(
        () => reject(new Error('Queued MeshNode ready work was not dispatched.')),
        1000
      ))
    ]);
    assert.equal(drainedBatches, 2);
  } finally {
    resolveFirstDispatch();
    await pump.dispose();
  }
});

test('backend mesh dispatch pump does not inherit the Spot turn that signaled ready', async () => {
  let readyHandler;
  let received = false;
  let releaseOwner;
  const ownerHeld = new Promise((resolve) => { releaseOwner = resolve; });
  let dispatchStarted;
  const started = new Promise((resolve) => { dispatchStarted = resolve; });
  let dispatchCompleted;
  const completed = new Promise((resolve) => { dispatchCompleted = resolve; });
  let reportedError;
  const claim = {
    recvBatch() {
      if (received) return { ok: false, records: [] };
      received = true;
      return { ok: true, records: [{ parts: [] }] };
    },
    release() {}
  };
  const node = {
    setReadyHandler(handler) { readyHandler = handler; },
    createReadyBatch() {
      return { reset() {}, takeClaim() { return claim; }, close() {} };
    },
    createReceiveBatch() {
      return { reset() {}, close() {} };
    },
    drainReady() {
      return {
        ok: true,
        hasResidue: false,
        records: [{ ownerKind: framework.ReadyOwnerKind.Spot }]
      };
    }
  };
  const serial = new ZLinkSpotSerialTurnExecutor();
  const pump = new backend.ZLinkMeshDispatchPump(node, {
    applicationJobQueue: applicationJobQueue(),
    async dispatch() {
      dispatchStarted();
      await serial.execute(() => dispatchCompleted());
    },
    reportError(error) { reportedError = error; }
  });

  try {
    pump.start();
    const owner = serial.execute(async () => {
      readyHandler(framework.ReadyDomain.Infrastructure);
      await ownerHeld;
    });
    await started;
    await new Promise((resolve) => setImmediate(resolve));
    assert.equal(reportedError, undefined);
    releaseOwner();
    await owner;
    await Promise.race([
      completed,
      new Promise((_, reject) => setTimeout(
        () => reject(new Error('Detached Mesh dispatch did not enter the Spot owner.')),
        1_000
      ))
    ]);
  } finally {
    releaseOwner();
    await pump.dispose();
  }
});

test('backend mesh dispatch pump does not claim unrelated owners behind a slow handler', async () => {
  let readyHandler;
  const owners = ['slow-owner', 'fast-owner'];
  let releaseSlow;
  const slowReleased = new Promise((resolve) => { releaseSlow = resolve; });
  let fastDispatched;
  const fastCompleted = new Promise((resolve) => { fastDispatched = resolve; });
  const node = {
    setReadyHandler(handler) { readyHandler = handler; },
    createReadyBatch(capacity) {
      assert.equal(capacity, 1);
      return {
        records: [],
        claims: [],
        reset() {
          this.records.length = 0;
          this.claims.length = 0;
        },
        takeClaim(index) { return this.claims[index]; },
        close() {}
      };
    },
    createReceiveBatch() {
      return { reset() {}, close() {} };
    },
    drainReady(_domain, batch) {
      const owner = owners.shift();
      if (owner === undefined) return { ok: true, hasResidue: false, records: [] };
      let received = false;
      batch.records.push({ ownerKind: framework.ReadyOwnerKind.Spot, owner });
      batch.claims.push({
        recvBatch() {
          if (received) return { ok: false, records: [] };
          received = true;
          return { ok: true, records: [{ parts: [], owner }] };
        },
        release() {}
      });
      return { ok: true, hasResidue: owners.length > 0, records: batch.records };
    }
  };
  const pump = new backend.ZLinkMeshDispatchPump(node, {
    applicationJobQueue: applicationJobQueue(),
    async dispatch(_owner, record) {
      if (record.owner === 'slow-owner') {
        await slowReleased;
      } else {
        fastDispatched();
      }
    }
  });

  try {
    pump.start();
    readyHandler(framework.ReadyDomain.Application);
    await Promise.race([
      fastCompleted,
      new Promise((_, reject) => setTimeout(
        () => reject(new Error('Unrelated owner remained blocked behind the slow handler.')),
        1_000
      ))
    ]);
  } finally {
    releaseSlow();
    await pump.dispose();
  }
});

test('mesh completion submission registers before a completion can arrive', async () => {
  const table = new ZLinkMeshCompletionTable();
  const complete = (low, text) => {
    const part = zlink.Message.from(Buffer.from(text));
    try {
      table.complete({
        operationId: { high: 1n, low },
        terminalResult: 0,
        failureErrno: 0,
        operationKind: 0,
        kindData: null,
        parts: [part]
      });
    } finally {
      part.close();
    }
  };

  const completion = table.submit(() => {
    queueMicrotask(() => complete(1n, 'first'));
    return { high: 1n, low: 1n };
  });
  const retained = await completion;
  try {
    assert.equal(retained.parts[0].data().toString(), 'first');
  } finally {
    closeMeshCompletion(retained);
  }
  complete(1n, 'late-duplicate');
  table.dispose();
});

test('mesh completion submission rejects when submit synchronously disposes the table', async () => {
  const table = new ZLinkMeshCompletionTable();

  await assert.rejects(
    table.submit(() => {
      table.dispose();
      return { high: 1n, low: 2n };
    }),
    /Mesh completion table is disposed/
  );
});

test('backend not-connected classification uses typed results instead of error text', () => {
  assert.equal(
    backend.isBackendNotConnectedError({
      result: backend.SubmitResult.NotConnected,
      message: 'arbitrary localized diagnostic'
    }),
    true
  );
  assert.equal(
    backend.isBackendNotConnectedError({
      result: backend.RequestResult.NotConnected,
      message: 'another diagnostic'
    }),
    true
  );
  assert.equal(
    backend.isBackendNotConnectedError(new Error('Transport endpoint is not connected')),
    false
  );
});

test('backend mesh record dispatcher routes node, spot, actor, and infrastructure records', async () => {
  const routed = [];
  const dispatcher = new backend.ZLinkMeshRecordDispatcher({
    node: (record) => routed.push(['node', record.kind]),
    spot: (owner, record) => routed.push(['spot', owner.ownerKind, record.kind]),
    actor: (owner, record) => routed.push(['actor', owner.ownerKind, record.kind]),
    completion: (record) => routed.push(['completion', record.kind]),
    sendReady: (record) => routed.push(['sendReady', record.kind]),
    transferControl: (record) => routed.push(['transferControl', record.kind])
  });
  const owner = (ownerKind) => ({ ownerKind });
  const record = (kind) => ({ kind });

  await dispatcher.dispatch(owner(framework.ReadyOwnerKind.Node), record(framework.ReceiveKind.ChannelRequest));
  await dispatcher.dispatch(owner(framework.ReadyOwnerKind.Spot), record(framework.ReceiveKind.SpotControl));
  await dispatcher.dispatch(owner(framework.ReadyOwnerKind.Actor), record(framework.ReceiveKind.ActorSend));
  await dispatcher.dispatch(owner(framework.ReadyOwnerKind.Actor), record(framework.ReceiveKind.ActorBinding));
  await framework.runZLinkExecutionArea('infrastructure', async () => {
    await dispatcher.dispatch(owner(framework.ReadyOwnerKind.Node), record(framework.ReceiveKind.Completion));
    await dispatcher.dispatch(owner(framework.ReadyOwnerKind.Node), record(framework.ReceiveKind.SendReady));
    await dispatcher.dispatch(owner(framework.ReadyOwnerKind.Node), record(framework.ReceiveKind.TransferControl));
  });

  assert.deepEqual(routed, [
    ['node', framework.ReceiveKind.ChannelRequest],
    ['spot', framework.ReadyOwnerKind.Spot, framework.ReceiveKind.SpotControl],
    ['actor', framework.ReadyOwnerKind.Actor, framework.ReceiveKind.ActorSend],
    ['actor', framework.ReadyOwnerKind.Actor, framework.ReceiveKind.ActorBinding],
    ['completion', framework.ReceiveKind.Completion],
    ['sendReady', framework.ReceiveKind.SendReady],
    ['transferControl', framework.ReceiveKind.TransferControl]
  ]);
  assert.throws(
    () => dispatcher.dispatch(owner(framework.ReadyOwnerKind.Spot), record(framework.ReceiveKind.ChannelSend)),
    /requires owner kind/
  );
  assert.throws(
    () => framework.runZLinkExecutionArea(
      'application',
      () => dispatcher.dispatch(owner(framework.ReadyOwnerKind.Node), record(framework.ReceiveKind.Completion))
    ),
    (error) => error.kind === framework.ZLinkFrameworkErrorKind.InvalidOperation
  );
});

test('MeshNode runtime manager owns lifecycle and forwards pull-dispatch records', async () => {
  const factory = new backend.ZLinkNodeBackendAdapterFactory();
  const context = factory.createChannelAdapter().createContext();
  const meshName = `runtime.dispatch.${process.pid}`;
  const receiverEndpoint = await reserveTcpEndpoint();
  const senderEndpoint = await reserveTcpEndpoint();
  class RuntimeDispatchNotice {
    handle() {}
  }
  const registration = framework.createFrameworkRegistrationWithBuilder((builder) => {
    const mesh = builder.addRouteMesh(meshName)
      .listen(receiverEndpoint)
      .routingId(`runtime-node-${process.pid}`);
    mesh.channel(meshName).server().addSendHandler(RuntimeDispatchNotice);
  });
  let resolveRecord;
  const received = new Promise((resolve) => {
    resolveRecord = resolve;
  });
  const runtime = new framework.ZLinkSpotNodeRuntimeManager({
    registration,
    backendAdapterFactory: factory,
    context,
    meshRecordDispatcher(_name, _owner, record) {
      resolveRecord({
        kind: record.kind,
        channelName: record.channelName,
        payload: record.parts[0].data().toString()
      });
    }
  });
  const sender = factory.createMeshAdapter().createMeshNode(context, {
    meshName,
    routingId: `runtime-sender-${process.pid}`,
    applicationJobQueue: applicationJobQueue()
  });
  let senderPump;

  try {
    await runtime.start();
    sender.setBind(senderEndpoint);
    sender.start();
    senderPump = new backend.ZLinkMeshDispatchPump(sender, {
      applicationJobQueue: applicationJobQueue(),
      dispatch() {},
      reportError(error) { throw error; }
    });
    senderPump.start();
    sender.connectPeer({
      endpoint: receiverEndpoint,
      expectedRid: runtime.primaryMeshNode.status().routingId
    });
    assert.equal(runtime.meshNodesByName.size, 1);
    assert.equal(await submitEventually(
      () => sender.sendToChannel(meshName, Buffer.from('runtime-pump'))
    ), zlink.SubmitResult.Ok);
    let timeout;
    const record = await Promise.race([
      received.finally(() => clearTimeout(timeout)),
      new Promise((_, reject) => {
        timeout = setTimeout(
          () => reject(new Error('Runtime MeshNode dispatch timed out.')),
          1000
        );
      })
    ]);
    assert.equal(record.kind, framework.ReceiveKind.ChannelSend);
    assert.equal(record.channelName, meshName);
    assert.equal(record.payload, 'runtime-pump');
  } finally {
    await senderPump?.dispose();
    sender.shutdown(1000);
    sender.close();
    await runtime.dispose();
    await context.dispose();
  }
});

test('Logical Multicast completion does not expose or record target admission results', async () => {
  const runtime = new framework.ZLinkSpotNodeRuntimeManager({
    registration: framework.createFrameworkRegistration({}),
    backendAdapterFactory: {},
    context: {}
  });
  let coreCalls = 0;
  runtime.publishers.set('play', {
    async publishAsync() {
      coreCalls += 1;
    }
  });

  const result = await runtime.publish('play', 'events', 'score', 'ProfileChanged', { sequence: 1 });

  assert.equal(coreCalls, 1);
  assert.deepEqual(result, { status: ZLinkSubmitStatus.Submitted });
});

test('Logical Multicast terminal completes while target processing remains blocked', async () => {
  const runtime = new framework.ZLinkSpotNodeRuntimeManager({
    registration: framework.createFrameworkRegistration({}),
    backendAdapterFactory: {},
    context: {}
  });
  let coreCalls = 0;
  let completeTarget;
  runtime.publishers.set('play', {
    publishAsync() {
      coreCalls += 1;
      return new Promise((resolve) => { completeTarget = resolve; });
    }
  });

  const result = await runtime.publish(
    'play', 'events', 'score', 'ProfileChanged', { sequence: 1 }
  );
  assert.equal(coreCalls, 1);
  assert.equal(result.status, ZLinkSubmitStatus.Submitted);
  assert.equal(runtime.activePublishes.has('play'), false);
  completeTarget();
});

test('Logical Multicast post-start failure does not change the caller terminal', async () => {
  const runtime = new framework.ZLinkSpotNodeRuntimeManager({
    registration: framework.createFrameworkRegistration({}),
    backendAdapterFactory: {},
    context: {}
  });
  let failTarget;
  runtime.publishers.set('play', {
    publishAsync() {
      return new Promise((_resolve, reject) => { failTarget = reject; });
    }
  });

  const result = await runtime.publish(
    'play', 'events', 'score', 'ProfileChanged', { sequence: 1 }
  );
  failTarget(new Error('target failed after handoff'));
  await new Promise((resolve) => setImmediate(resolve));
  assert.equal(result.status, ZLinkSubmitStatus.Submitted);
});

test('Logical Multicast shutdown after handoff does not change the caller terminal', async () => {
  const runtime = new framework.ZLinkSpotNodeRuntimeManager({
    registration: framework.createFrameworkRegistration({}),
    backendAdapterFactory: {},
    context: {}
  });
  let coreCalls = 0;
  runtime.publishers.set('play', {
    publishAsync() {
      coreCalls += 1;
      return new Promise(() => undefined);
    },
    async close() {}
  });

  const submitted = await runtime.publish(
    'play', 'events', 'score', 'ProfileChanged', { sequence: 1 }
  );
  await runtime.dispose();
  assert.equal(submitted.status, ZLinkSubmitStatus.Submitted);
  await assert.rejects(
    runtime.publish('play', 'events', 'score', 'ProfileChanged', { sequence: 2 }),
    (error) => {
      assert.equal(error instanceof framework.ZLinkFrameworkException, true);
      assert.equal(error.kind, framework.ZLinkFrameworkErrorKind.ShuttingDown);
      return true;
    }
  );
  assert.equal(coreCalls, 1);
  assert.equal(runtime.activePublishes.has('play'), false);
});

test('Logical Multicast accepts zero subscribers as normal completion', async () => {
  const runtime = new framework.ZLinkSpotNodeRuntimeManager({
    registration: framework.createFrameworkRegistration({}),
    backendAdapterFactory: {},
    context: {}
  });
  runtime.publishers.set('play', {
    async publishAsync() {
    }
  });

  const result = await runtime.publish(
    'play', 'events', 'score', 'ProfileChanged', { sequence: 1 }
  );

  assert.equal(result.status, ZLinkSubmitStatus.Submitted);
});

test('Logical Multicast releases its handoff slot after envelope encoding fails', async () => {
  const runtime = new framework.ZLinkSpotNodeRuntimeManager({
    registration: framework.createFrameworkRegistration({}),
    backendAdapterFactory: {},
    context: {}
  });
  let coreCalls = 0;
  runtime.publishers.set('play', {
    async publishAsync() {
      coreCalls += 1;
    }
  });
  const cyclic = {};
  cyclic.self = cyclic;

  await assert.rejects(
    runtime.publish('play', 'events', 'score', 'ProfileChanged', cyclic),
    /circular/i
  );
  assert.equal(coreCalls, 0);

  const result = await runtime.publish(
    'play', 'events', 'score', 'ProfileChanged', { sequence: 2 }
  );
  assert.equal(coreCalls, 1);
  assert.equal(result.status, ZLinkSubmitStatus.Submitted);
});

test('Logical Multicast abort before handoff calls Core zero times', async () => {
  const runtime = new framework.ZLinkSpotNodeRuntimeManager({
    registration: framework.createFrameworkRegistration({}),
    backendAdapterFactory: {},
    context: {}
  });
  let coreCalls = 0;
  runtime.publishers.set('play', {
    async publishAsync() {
      coreCalls += 1;
      throw new Error('must not run');
    }
  });
  const controller = new AbortController();
  const reason = new Error('cancel before commit');
  controller.abort(reason);

  await assert.rejects(
    runtime.publish('play', 'events', 'score', 'ProfileChanged', { sequence: 1 }, controller.signal),
    (error) => error === reason
  );
  assert.equal(coreCalls, 0);
});

test('Logical Multicast abort after Core start preserves committed completion', async () => {
  const runtime = new framework.ZLinkSpotNodeRuntimeManager({
    registration: framework.createFrameworkRegistration({}),
    backendAdapterFactory: {},
    context: {}
  });
  let coreCalls = 0;
  let complete;
  let forwardedSignal;
  runtime.publishers.set('play', {
    publishAsync(channelName, topic, parts, options, signal) {
      coreCalls += 1;
      forwardedSignal = signal;
      return new Promise((resolve) => { complete = resolve; });
    }
  });
  const controller = new AbortController();
  const pending = runtime.publish(
    'play', 'events', 'score', 'ProfileChanged', { sequence: 1 }, controller.signal
  );
  assert.equal(forwardedSignal, undefined);
  controller.abort(new Error('late abort'));

  const result = await pending;
  assert.equal(coreCalls, 1);
  assert.equal(result.status, ZLinkSubmitStatus.Submitted);
  complete();
});

test('Logical Multicast binding commit preserves admission after post-start abort', async () => {
  const fixture = path.join(__dirname, 'fixtures', 'logical-multicast-commit-child.js');
  const child = spawn(process.execPath, [fixture], {
    env: { ...process.env, UV_THREADPOOL_SIZE: '1' },
    stdio: ['ignore', 'pipe', 'pipe']
  });
  let stdout = '';
  let stderr = '';
  child.stdout.setEncoding('utf8');
  child.stderr.setEncoding('utf8');
  child.stdout.on('data', (chunk) => { stdout += chunk; });
  child.stderr.on('data', (chunk) => { stderr += chunk; });
  const [code, signal] = await once(child, 'exit');

  assert.equal(signal, null, stderr);
  assert.equal(code, 0, stderr);
  assert.match(stdout, /logical-multicast-commit-ok/);
});

test('framework host dispatches a MeshNode channel record through registered handler lifecycle', async () => {
  const meshName = `host.dispatch.${process.pid}`;
  const receiverEndpoint = await reserveTcpEndpoint();
  const senderEndpoint = await reserveTcpEndpoint();
  let resolveHandled;
  const handled = new Promise((resolve) => {
    resolveHandled = resolve;
  });
  class MeshNotice {
    handle(message, context) {
      resolveHandled({ message, context });
    }
  }
  const registration = framework.createFrameworkRegistrationWithBuilder((builder) => {
    const mesh = builder.addRouteMesh(meshName)
      .listen(receiverEndpoint)
      .routingId(`host-dispatch-node-${process.pid}`);
    mesh.channel(meshName).server().addSendHandler(MeshNotice);
  });
  const host = new framework.ZLinkFrameworkRuntimeHost({ registration });
  const factory = new backend.ZLinkNodeBackendAdapterFactory();
  const context = factory.createChannelAdapter().createContext();
  const sender = factory.createMeshAdapter().createMeshNode(context, {
    meshName,
    routingId: `host-dispatch-sender-${process.pid}`,
    applicationJobQueue: applicationJobQueue()
  });
  let senderPump;

  try {
    await host.start();
    const node = host.requirePrimaryMeshNode();
    sender.setBind(senderEndpoint);
    sender.start();
    senderPump = new backend.ZLinkMeshDispatchPump(sender, {
      applicationJobQueue: applicationJobQueue(),
      dispatch() {},
      reportError(error) { throw error; }
    });
    senderPump.start();
    sender.connectPeer({
      endpoint: receiverEndpoint,
      expectedRid: node.status().routingId
    });
    assert.equal(await submitEventually(
      () => sender.sendToChannel(
        meshName,
        channelEnvelope.encodeChannelEnvelopeParts(
          3,
          meshName,
          'MeshNotice',
          { value: 'handled' }
        )
      )
    ), zlink.SubmitResult.Ok);
    let timeout;
    const result = await Promise.race([
      handled.finally(() => clearTimeout(timeout)),
      new Promise((_, reject) => {
        timeout = setTimeout(() => reject(new Error('Mesh channel handler dispatch timed out.')), 1000);
      })
    ]);
    assert.deepEqual(result.message, { value: 'handled' });
    assert.equal(result.context.channelName, meshName);
    assert.equal(result.context.packetName, 'MeshNotice');
  } finally {
    await senderPump?.dispose();
    sender.shutdown(1000);
    sender.close();
    await context.dispose();
    await host.stop();
  }
});

test('framework host dispatches MeshNode node-direct send and request records', async () => {
  const meshName = `host.node-direct.${process.pid}`;
  let resolveHandled;
  const handled = new Promise((resolve) => {
    resolveHandled = resolve;
  });
  class DirectNotice {
    handle(message, context) {
      resolveHandled({ message, context });
    }
  }
  class DirectQuery {
    handle(message, context) {
      return { echoed: message.value, source: context.sourceNodeRid };
    }
  }
  class DirectChannelNotice {
    handle() {}
  }
  const registration = framework.createFrameworkRegistrationWithBuilder((builder) => {
    const mesh = builder.addRouteMesh(meshName)
      .listen(`inproc://${meshName}`)
      .routingId(`host-node-direct-${process.pid}`)
      .addRouteSendHandler(DirectNotice)
      .addRouteRequestHandler(DirectQuery);
    mesh.channel(`${meshName}.direct`).server().addSendHandler(DirectChannelNotice);
  });
  const host = new framework.ZLinkFrameworkRuntimeHost({ registration });

  try {
    await host.start();
    const node = host.requirePrimaryMeshNode();
    const target = node.status().routingId;
    const sendParts = channelEnvelope.encodeChannelEnvelopeParts(
      3,
      meshName,
      'DirectNotice',
      { value: 'sent' }
    ).map((part) => zlink.Message.from(part));
    await host.dispatchMeshRecord(meshName, { ownerKind: framework.ReadyOwnerKind.Node }, {
      kind: framework.ReceiveKind.NodeSend,
      sourceNodeRid: target,
      parts: sendParts
    });
    const sent = await handled;
    assert.deepEqual(sent.message, { value: 'sent' });
    assert.equal(sent.context.meshName, meshName);
    assert.equal(sent.context.channelName, undefined);
    assert.equal(String(sent.context.sourceNodeRid), String(target));

    const requestParts = channelEnvelope.encodeChannelEnvelopeParts(
      1,
      meshName,
      'DirectQuery',
      { value: 'asked' }
    ).map((part) => zlink.Message.from(part));
    let replyParts;
    await host.dispatchMeshRecord(meshName, { ownerKind: framework.ReadyOwnerKind.Node }, {
      kind: framework.ReceiveKind.NodeRequest,
      sourceNodeRid: target,
      parts: requestParts,
      reply(parts) {
        replyParts = parts.map((part) => zlink.Message.from(part));
        return zlink.SubmitResult.Ok;
      }
    });
    try {
      const reply = channelEnvelope.decodeChannelEnvelope(replyParts);
      assert.deepEqual(JSON.parse(reply.payload.toString()), {
        echoed: 'asked',
        source: String(target)
      });
    } finally {
      for (const part of sendParts) part.close();
      for (const part of requestParts) part.close();
      for (const part of replyParts) part.close();
    }
  } finally {
    await host.stop();
  }
});

test('backend adapter creates context and core socket wrappers through public binding API', async () => {
  const factory = new backend.ZLinkNodeBackendAdapterFactory();
  const channel = factory.createChannelAdapter();
  const context = channel.createContext();
  const disposables = [];

  try {
    const dealer = channel.createDealerSocket(context);
    const router = channel.createRouterSocket(context);
    const publisher = channel.createPublisherSocket(context);
    const subscriber = channel.createSubscriberSocket(context);
    const topicMessage = channel.createTopicMessage();
    const subscriberPoller = channel.createReadablePoller(subscriber);
    const streamAdapter = factory.createStreamAdapter();
    const stream = streamAdapter.createStreamSocket(context);
    const streamPoller = streamAdapter.createReadablePoller(stream);

    disposables.push(dealer, router, publisher, subscriber, subscriberPoller, stream, streamPoller);

    assert.equal(Array.isArray(topicMessage.parts), true);
    assert.equal(typeof dealer.dispose, 'function');
    assert.equal(typeof router.dispose, 'function');
    assert.equal(typeof publisher.dispose, 'function');
    assert.equal(typeof subscriber.dispose, 'function');
    assert.equal(typeof subscriberPoller.wait, 'function');
    assert.equal(typeof subscriberPoller.dispose, 'function');
    assert.equal(typeof streamPoller.wait, 'function');
    assert.equal(typeof streamPoller.dispose, 'function');
    assert.equal(typeof stream.dispose, 'function');
  } finally {
    for (const disposable of disposables.reverse()) {
      await disposable.dispose();
    }
    await context.dispose();
  }
});

test('PUB backend uses only the synchronous 0.13 publish operation', () => {
  const calls = [];
  const native = {
    close() {},
    publish(topic) {
      calls.push(`publish:${topic}`);
      return {
        message(part) {
          calls.push(`part:${Buffer.from(part).toString()}`);
          return this;
        },
        submit() { calls.push('submit'); }
      };
    }
  };
  const publisher = wrapSocket(native);
  assert.equal(publisher.publish('raw', [Buffer.from('one'), Buffer.from('two')]), undefined);
  assert.equal(publisher.publishAsync, undefined);
  assert.deepEqual(calls, [
    'publish:raw', 'part:one', 'part:two', 'submit'
  ]);
});

test('backend adapter unwraps SpotNode when attaching stream SessionRelay', async () => {
  const factory = new backend.ZLinkNodeBackendAdapterFactory();
  const channel = factory.createChannelAdapter();
  const spotAdapter = factory.createSpotAdapter();
  const streamAdapter = factory.createStreamAdapter();
  const context = channel.createContext();
  const spotNode = spotAdapter.createSpotNode(context, 3);
  const stream = streamAdapter.createStreamSocket(context);

  try {
  } finally {
    await stream.dispose();
    await spotNode.dispose();
    await context.dispose();
  }
});

test('backend stream bind converts public string actor node RID to native RoutingId', async () => {
  const factory = new backend.ZLinkNodeBackendAdapterFactory();
  const channel = factory.createChannelAdapter();
  const spotAdapter = factory.createSpotAdapter();
  const streamAdapter = factory.createStreamAdapter();
  const context = channel.createContext();
  const sessionNode = spotAdapter.createSpotNode(context, 3);
  const playNode = spotAdapter.createSpotNode(context, 3);
  const stream = streamAdapter.createStreamSocket(context);

  try {
    sessionNode.setRoutingId('backend-session-node');
    playNode.setRoutingId('backend-play-node');
    const actorRef = playNode.createActor('backend-player');

    await stream.bindActor(
      'backend-session',
      {
        nodeRid: String(actorRef.nodeRid),
        actorId: actorRef.actorId,
        generation: actorRef.generation
      },
      1000
    );
  } finally {
    await stream.dispose();
    await playNode.dispose();
    await sessionNode.dispose();
    await context.dispose();
  }
});

test('backend adapter converts public string route bridge target RIDs to native RoutingId', async () => {
  const factory = new backend.ZLinkNodeBackendAdapterFactory();
  const channel = factory.createChannelAdapter();
  const spotAdapter = factory.createSpotAdapter();
  const context = channel.createContext();
  const spotNode = spotAdapter.createSpotNode(context, 3);
  const router = channel.createRouterSocket(context);
  const bridge = spotNode.createRouteBridge();

  try {
    router.setRoutingId('backend-bridge-source');
    bridge.attachRouterChannel('mesh', router, { capabilities: 3 });

    assert.equal(typeof bridge.send('mesh', 'backend-bridge-target', 'backend-bridge-spot').message, 'function');
    assert.equal(typeof bridge.request('mesh', 'backend-bridge-target', 'backend-bridge-spot').message, 'function');
  } finally {
    await bridge.dispose();
    await router.dispose();
    await spotNode.dispose();
    await context.dispose();
  }
});

test('channel runtime connects route mesh peers before bind and spot route bridge attach', () => {
  const calls = [];
  const registration = framework.createFrameworkRegistrationWithBuilder((builder) => {
    builder.addRouteMesh('room.route')
      .enableServer('tcp://0.0.0.0:9410')
      .enableClient('tcp://127.0.0.1:9410');
    builder.addRouteMesh('room')
      .listen('tcp://0.0.0.0:9411', 'room-node');
  });
  const routeSocket = {
    nativeInstance: {},
    setChannelName(channelName) { calls.push(`route:setChannelName:${channelName}`); },
    setRoutingId(routingId) { calls.push(`route:setRoutingId:${routingId}`); },
    connect(endpoint) { calls.push(`route:connect:${endpoint}`); },
    bind(endpoint) { calls.push(`route:bind:${endpoint}`); },
    disconnect() {},
    recv() { return undefined; },
    async dispose() {}
  };
  const spotNode = {
    routingId: 'room-node',
    createRouteBridge() {
      calls.push('spot:createRouteBridge');
      return {
        attachRouterChannel(channelName) { calls.push(`bridge:attachRouter:${channelName}`); },
        async dispose() {}
      };
    }
  };
  const runtime = new framework.ZLinkChannelRuntimeManager(
    registration,
    {
      createRouterSocket() {
        calls.push('route:createRouter');
        return routeSocket;
      }
    },
    { nativeInstance: {}, shutdown() {}, async dispose() {} }
  );

  runtime.bindRouteMeshRouters();
  runtime.setSpotNodes(new Map([['room', spotNode]]));
  runtime.start({
    errorSink: { report() {} },
    run(_name, _task) {
      return Promise.resolve();
    }
  });

  assert.deepEqual(calls, [
    'route:createRouter',
    'route:setChannelName:room.route',
    'route:connect:tcp://127.0.0.1:9410',
    'route:bind:tcp://0.0.0.0:9410',
    'spot:createRouteBridge',
    'bridge:attachRouter:room.route'
  ]);
});

test('backend adapter normalizes missing SpotNode actor lookup to undefined', async () => {
  const factory = new backend.ZLinkNodeBackendAdapterFactory();
  const channel = factory.createChannelAdapter();
  const spotAdapter = factory.createSpotAdapter();
  const context = channel.createContext();
  const spotNode = spotAdapter.createSpotNode(context, 3);

  try {
    assert.equal(spotNode.actorLookup('missing-actor'), undefined);

    const actorRef = spotNode.createActor('existing-actor');
    assert.equal(actorRef.actorId, 'existing-actor');
    assert.equal(typeof actorRef.generation, 'bigint');
    assert.deepEqual(spotNode.actorLookup('existing-actor'), actorRef);
  } finally {
    await spotNode.dispose();
    await context.dispose();
  }
});

test('backend adapter submits actor destroy and closes actor bound sessions through public binding operations', async () => {
  const factory = new backend.ZLinkNodeBackendAdapterFactory();
  const channel = factory.createChannelAdapter();
  const spotAdapter = factory.createSpotAdapter();
  const context = channel.createContext();
  const spotNode = spotAdapter.createSpotNode(context, 3);
  const calls = [];
  let replyClosed = false;

  try {
    spotNode.nativeInstance.createActor = (actorId) => ({
      actorRef: { nodeRid: zlink.RoutingId.from('backend-node'), actorId, generation: 1n },
      closeBoundSession(timeoutMs) {
        calls.push({ kind: 'closeBoundSession', timeoutMs });
      }
    });
    spotNode.nativeInstance.destroyActor = (actorRef) => ({
      timeout(timeoutMs) {
        calls.push({ kind: 'destroyTimeout', actorRef, timeoutMs });
        return this;
      },
      submit() {
        calls.push({ kind: 'destroySubmit' });
        return Promise.resolve([{ close() { replyClosed = true; } }]);
      }
    });

    const actorRef = spotNode.createActor('backend-destroy');
    await spotNode.closeActorBoundSession(actorRef, 17);
    await spotNode.destroyActor(actorRef, 23);

    assert.deepEqual(calls.map((call) => call.kind), [
      'closeBoundSession',
      'destroyTimeout',
      'destroySubmit'
    ]);
    assert.equal(calls[0].timeoutMs, 17);
    assert.equal(calls[1].timeoutMs, 23);
    assert.equal(calls[1].actorRef.actorId, 'backend-destroy');
    assert.equal(replyClosed, true);
    await assert.rejects(
      spotNode.closeActorBoundSession(actorRef, 0),
      (error) => error instanceof zlink.ConfigError && error.result === zlink.ConfigResult.NotFound
    );
  } finally {
    await spotNode.dispose();
    await context.dispose();
  }
});

test('backend adapter aborts actor destroy waits and closes late reply messages', async () => {
  const factory = new backend.ZLinkNodeBackendAdapterFactory();
  const channel = factory.createChannelAdapter();
  const spotAdapter = factory.createSpotAdapter();
  const context = channel.createContext();
  const spotNode = spotAdapter.createSpotNode(context, 3);
  const abort = new AbortController();
  let completeDestroy;
  let replyClosed = false;

  try {
    spotNode.nativeInstance.createActor = (actorId) => ({
      actorRef: { nodeRid: zlink.RoutingId.from('backend-node'), actorId, generation: 1n },
      closeBoundSession() {}
    });
    spotNode.nativeInstance.destroyActor = () => ({
      timeout() { return this; },
      submit() {
        return new Promise((resolve) => {
          completeDestroy = resolve;
        });
      }
    });

    const actorRef = spotNode.createActor('backend-abort-destroy');
    const destroy = spotNode.destroyActor(actorRef, 1000, abort.signal);
    const reason = new Error('cancel backend actor destroy');
    abort.abort(reason);
    await assert.rejects(destroy, reason);

    completeDestroy([{ close() { replyClosed = true; } }]);
    await new Promise((resolve) => setImmediate(resolve));
    assert.equal(replyClosed, true);
    await assert.rejects(
      spotNode.closeActorBoundSession(actorRef, 0),
      (error) => error instanceof zlink.ConfigError && error.result === zlink.ConfigResult.NotFound
    );
  } finally {
    await spotNode.dispose();
    await context.dispose();
  }
});

test('backend adapter submits SpotNode actor bound-session send operation', async () => {
  const factory = new backend.ZLinkNodeBackendAdapterFactory();
  const channel = factory.createChannelAdapter();
  const spotAdapter = factory.createSpotAdapter();
  const context = channel.createContext();
  const spotNode = spotAdapter.createSpotNode(context, 3);
  const frame = zlink.Message.from(Buffer.from('frame'));

  try {
    const actorRef = spotNode.createActor('unbound-actor');
    assert.throws(
      () => spotNode.sendActorBoundSession(actorRef, [frame], 1),
      /actor bound session send failed|not found|No such file|ENOENT|NOT_FOUND/i
    );
  } finally {
    frame.close();
    await spotNode.dispose();
    await context.dispose();
  }
});

test('backend adapter submits SpotNode actor send through async completion callback', async () => {
  const factory = new backend.ZLinkNodeBackendAdapterFactory();
  const channel = factory.createChannelAdapter();
  const spotAdapter = factory.createSpotAdapter();
  const context = channel.createContext();
  const spotNode = spotAdapter.createSpotNode(context, 3);
  const frame = zlink.Message.from(Buffer.from('actor-send'));
  const actorRef = { nodeRid: 'backend-actor-node', actorId: 'backend-actor', generation: 1n };
  const calls = [];

  try {
    spotNode.nativeInstance.sendToActorCallback = (actor, parts, callback, flags, timeoutMs) => {
      calls.push({ actor, parts, flags, timeoutMs });
      queueMicrotask(() => callback(zlink.RequestResult.Ok, []));
      return true;
    };

    assert.equal(await spotNode.sendToActor(actorRef, [frame], zlink.SendFlags.None), true);
    assert.equal(calls.length, 1);
    assert.equal(calls[0].actor.actorId, 'backend-actor');
    assert.equal(calls[0].parts.length, 1);
  } finally {
    frame.close();
    await spotNode.dispose();
    await context.dispose();
  }
});

test('backend adapter submits SpotNode actor request builder and wires callback', async () => {
  const factory = new backend.ZLinkNodeBackendAdapterFactory();
  const channel = factory.createChannelAdapter();
  const spotAdapter = factory.createSpotAdapter();
  const context = channel.createContext();
  const spotNode = spotAdapter.createSpotNode(context, 3);
  const frame = zlink.Message.from(Buffer.from('actor-request'));
  const actorRef = { nodeRid: 'backend-actor-node', actorId: 'backend-actor', generation: 1n };
  const calls = [];

  try {
    spotNode.nativeInstance.requestToActor = (actor) => ({
      message(part) {
        calls.push({ kind: 'message', actor, part });
        return this;
      },
      flags(flags) {
        calls.push({ kind: 'flags', flags });
        return this;
      },
      timeout(timeoutMs) {
        calls.push({ kind: 'timeout', timeoutMs });
        return this;
      },
      submit(callback) {
        calls.push({ kind: 'submit' });
        callback(zlink.RequestResult.Ok, []);
        return true;
      }
    });

    let completed = false;
    const accepted = spotNode.requestToActor(
      actorRef,
      [frame],
      (result, parts) => {
        completed = true;
        assert.equal(result, zlink.RequestResult.Ok);
        assert.deepEqual(parts, []);
      },
      zlink.SendFlags.None,
      123
    );

    assert.equal(accepted, true);
    assert.equal(completed, true);
    assert.deepEqual(calls.map((call) => call.kind), ['message', 'timeout', 'submit']);
  } finally {
    frame.close();
    await spotNode.dispose();
    await context.dispose();
  }
});

test('backend router recv normalizes transient route recv invalid handle to no message', async () => {
  const factory = new backend.ZLinkNodeBackendAdapterFactory();
  const channel = factory.createChannelAdapter();
  const context = channel.createContext();
  const router = channel.createRouterSocket(context);

  try {
    await router.dispose();
    assert.equal(router.recv(1), undefined);
  } finally {
    await context.dispose();
  }
});

test('backend socket wrapper treats missing route disconnect as idempotent cleanup', () => {
  const error = new zlink.ConfigError(zlink.ConfigResult.NotFound, 2);

  assert.equal(backend.isDisconnectRouteNotFoundError(error), true);
});

test('subscriber receive loop never blocks the Node event loop while polling', async () => {
  const waits = [];
  const loop = new framework.ZLinkSubscriberReceiveLoop(
    {
      createReadablePoller() {
        return {
          wait(timeoutMs) {
            waits.push(timeoutMs);
            return false;
          },
          dispose() {}
        };
      }
    },
    {},
    { async dispatch() {} },
    applicationJobQueue()
  );

  const running = loop.run();
  while (waits.length === 0) await new Promise((resolve) => setImmediate(resolve));
  await loop.stop();
  await running;

  assert.ok(waits.length > 0);
  assert.deepEqual([...new Set(waits)], [0]);
});

test('subscriber receive loop keeps receiving while an earlier handler is awaiting', async () => {
  const queued = [messageRecord('first'), messageRecord('second')];
  let releaseFirst;
  const firstPending = new Promise((resolve) => { releaseFirst = resolve; });
  let observeSecond;
  const secondObserved = new Promise((resolve) => { observeSecond = resolve; });
  const queue = applicationJobQueue(1n);
  const loop = new framework.ZLinkSubscriberReceiveLoop(
    {
      createReadablePoller() {
        return {
          wait() { return queued.length > 0; },
          dispose() {}
        };
      },
      createTopicMessage() { return { topic: '', parts: [] }; }
    },
    {
      subscribe(target) {
        const next = queued.shift();
        if (next === undefined) return false;
        target.topic = next.topic;
        target.parts = next.parts;
        return true;
      }
    },
    {
      async dispatch(topicMessage) {
        releaseApplicationJobPermitBeforeHandler();
        if (topicMessage.topic === 'first') await firstPending;
        if (topicMessage.topic === 'second') observeSecond();
      }
    },
    queue
  );

  const running = loop.run();
  try {
    await Promise.race([
      secondObserved,
      new Promise((_, reject) => setTimeout(
        () => reject(new Error('Second subscriber record waited for the first handler.')),
        1_000
      ))
    ]);
  } finally {
    releaseFirst();
    await loop.stop();
    await running;
  }
  assert.equal(queue.snapshot().effectiveMaxQueuedApplicationJobs, 1n);
  assert.equal(queue.snapshot().peakPermitsInUse, 1n);
  assert.equal(queue.snapshot().permitsInUse, 0n);
});

function messageRecord(topic) {
  return {
    topic,
    parts: [{
      size() { return 0; },
      data() { return Buffer.alloc(0); },
      close() {}
    }, {
      size() { return 0; },
      data() { return Buffer.alloc(0); },
      close() {}
    }]
  };
}
