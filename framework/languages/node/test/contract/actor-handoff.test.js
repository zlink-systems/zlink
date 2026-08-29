const assert = require('node:assert/strict');
const test = require('node:test');

const zlink = require('@zlink-systems/zlink');
const framework = require('../../packages/framework/dist/internal');
const streamProtocol = require('../../packages/framework/dist/runtime/streams/protocol');
const actorHandoff = require('../../packages/framework/dist/runtime/actors/actor-handoff');
const actorRelayWire = require(
  '../../packages/framework/dist/runtime/actors/actor-packet-relay-wire'
);
const messageFollow = require(
  '../../packages/framework/dist/runtime/actors/actor-message-follow-context'
);
const routingIds = require('../../packages/framework/dist/runtime/routing-id');
const {
  ZLinkActorSerialExecutor
} = require('../../packages/framework/dist/runtime/actors/actor-mailbox');
const {
  ZLinkSpotSerialTurnExecutor
} = require('../../packages/framework/dist/runtime/spots/spot-serial-turn-executor');
const {
  ZLinkActorTransferRuntime
} = require('../../packages/framework/dist/runtime/host/actor-transfer-runtime');
const {
  ApplicationJobQueue,
  resolveApplicationJobQueueConfiguration
} = require('../../packages/framework/dist/runtime/host/application-job-queue');
const {
  runWithApplicationJobPermit
} = require('../../packages/framework/dist/runtime/application-jobs/application-job-queue-scope');

function frame(value) {
  return [zlink.Message.from(`header:${value}`), zlink.Message.from(value)];
}

function actorRef(generation = 1n) {
  return {
    nodeRid: zlink.RoutingId.from('source'),
    actorId: 'actor-1',
    objectGeneration: generation,
    meshName: 'mesh'
  };
}

function target(name = 'target') {
  return {
    routerChannelId: 'mesh',
    targetNodeRid: zlink.RoutingId.from(`${name}-node`),
    spotId: zlink.RoutingId.from(`${name}-spot`),
    spotKind: framework.ZLinkSpotKind.User,
    targetNodeGeneration: 1n,
    authorityOwnerGeneration: 2n,
    targetOwnerId: `${name}-owner`,
    ownerLeaseGeneration: 1n
  };
}

function targetActorRef(name = 'target', generation = 1n) {
  return {
    nodeRid: zlink.RoutingId.from(`${name}-node`),
    actorId: 'actor-1',
    objectGeneration: generation,
    meshName: 'mesh'
  };
}

function ownerFence(name, authorityOwnerGeneration) {
  return messageFollow.ownerFence({
    ownerId: `${name}-owner`,
    ownerLeaseGeneration: 1n,
    nodeRid: `${name}-node`,
    nodeGeneration: 1n,
    authorityOwnerGeneration
  });
}

function sourceOwnerFence(authorityOwnerGeneration = 1n) {
  return messageFollow.ownerFence({
    ownerId: 'source-owner',
    ownerLeaseGeneration: 1n,
    nodeRid: 'source-node',
    nodeGeneration: 1n,
    authorityOwnerGeneration
  });
}

function contextRef(parts, options = {}) {
  const source = options.sourceOwner ?? sourceOwnerFence();
  const context = {
    operationId: options.operationId ?? '11111111111111111111111111111111',
    objectGeneration: String(options.objectGeneration ?? 1n),
    sourceOwner: source,
    targetOwner: options.targetOwner ?? source,
    deadlineUnixMs: options.deadlineUnixMs,
    correlationId: options.request
      ? options.correlationId ?? '22222222222222222222222222222222'
      : undefined,
    replyRouteId: options.request
      ? options.replyRouteId ?? '33333333333333333333333333333333'
      : undefined,
    request: options.request ?? false,
    hopCount: options.hopCount ?? 0,
    visitedOwners: options.visitedOwners
      ?? [messageFollow.messageFollowOwnerFenceKey(options.targetOwner ?? source)],
    payloadChecksumSha256: messageFollow.actorMessageFollowPayloadChecksum(parts)
  };
  const decoded = messageFollow.decodeActorMessageFollowContext(context);
  return messageFollow.attachActorMessageFollowContext(
    options.actorRef ?? actorRef(BigInt(context.objectGeneration)),
    decoded
  );
}

function harness(messageFollowDurationMs = 30) {
  const followed = [];
  const messageFollowPayloads = [];
  const markers = [];
  let currentGeneration = 2n;
  let currentNodeRid = 'target-node';
  let currentOwner = ownerFence('target', 2n);
  let sourceStateAvailable = true;
  let sourceLookupCount = 0;
  let replyHostOwnerId = 'source-owner';
  let replyHostOwnerLeaseGeneration = 1n;
  let replyHostNodeRid = 'source-node';
  let replyHostNodeRidHex = Buffer.from(replyHostNodeRid).toString('hex');
  let replyHostNodeGeneration = 1n;
  let requestSource = {
    meshName: 'mesh',
    objectGeneration: 1n,
    ownerId: 'source-owner',
    ownerLeaseGeneration: 1n,
    nodeRid: 'source-node',
    nodeGeneration: 1n,
    authorityOwnerGeneration: 1n
  };
  const transport = {
    async sendToSpot(_target, payload) {
      messageFollowPayloads.push(payload);
      followed.push(Buffer.from(payload.payload, 'base64').toString());
    },
    async requestToSpot() {
      return { ok: true };
    }
  };
  const coordinator = new framework.ZLinkActorHandoffCoordinator({
    routedTransport: transport,
    messageFollowDurationMs,
    isStaleActorRef: (_actorId, ref) => ref.objectGeneration !== currentGeneration,
    isCurrentActorRef: (_actorId, ref) =>
      ref.objectGeneration === currentGeneration
      && String(ref.nodeRid) === currentNodeRid,
    isCurrentHandoffTarget: (_actorId, spotId) => spotId === 'target-spot',
    currentOwnerFence: () => currentOwner,
    requestSource: () => {
      sourceLookupCount += 1;
      if (!sourceStateAvailable) throw new Error('Actor source state was removed.');
      return requestSource;
    },
    validateReplySource: (source) =>
      source.ownerId === replyHostOwnerId
      && source.ownerLeaseGeneration === replyHostOwnerLeaseGeneration
      && source.nodeRid === replyHostNodeRid
      && source.nodeRidHex === replyHostNodeRidHex
      && source.nodeGeneration === replyHostNodeGeneration,
    onMarker: (marker, actorId, index) => markers.push({ marker, actorId, index })
  });
  return {
    coordinator,
    followed,
    messageFollowPayloads,
    markers,
    setCurrentGeneration(value) { currentGeneration = value; },
    setCurrentNodeRid(value) { currentNodeRid = value; },
    setCurrentOwner(value) { currentOwner = value; },
    setRequestSource(value) {
      requestSource = { meshName: 'mesh', ...value };
      sourceStateAvailable = true;
      replyHostOwnerId = requestSource.ownerId;
      replyHostOwnerLeaseGeneration = requestSource.ownerLeaseGeneration;
      replyHostNodeRid = requestSource.nodeRid;
      replyHostNodeRidHex = requestSource.nodeRidHex
        ?? Buffer.from(requestSource.nodeRid).toString('hex');
      replyHostNodeGeneration = requestSource.nodeGeneration;
    },
    removeSourceState() { sourceStateAvailable = false; },
    setReplyHostOwnerLeaseGeneration(value) { replyHostOwnerLeaseGeneration = value; },
    setReplyHostNodeGeneration(value) { replyHostNodeGeneration = value; },
    sourceLookupCount() { return sourceLookupCount; }
  };
}

test('handoff admission requires committed source and target owner fences', () => {
  let sourceLookupCount = 0;
  const invalidGeneration = new framework.ZLinkActorHandoffCoordinator({
    routedTransport: { async sendToSpot() {}, async requestToSpot() {} },
    requestSource() {
      sourceLookupCount += 1;
      return {
        meshName: 'mesh',
        objectGeneration: 1n,
        ownerId: 'source-owner',
        ownerLeaseGeneration: 1n,
        nodeRid: 'source-node',
        nodeGeneration: 1n,
        authorityOwnerGeneration: 1n
      };
    },
    validateReplySource: () => true
  });
  assert.throws(
    () => invalidGeneration.begin('actor-1', 0n),
    /positive source ObjectGeneration/u
  );
  assert.equal(sourceLookupCount, 0);
  assert.equal(invalidGeneration.isActive('actor-1'), false);

  const replacedSource = new framework.ZLinkActorHandoffCoordinator({
    routedTransport: { async sendToSpot() {}, async requestToSpot() {} },
    requestSource: () => ({
      meshName: 'mesh',
      objectGeneration: 2n,
      ownerId: 'replacement-owner',
      ownerLeaseGeneration: 2n,
      nodeRid: 'replacement-node',
      nodeGeneration: 2n,
      authorityOwnerGeneration: 2n
    }),
    validateReplySource: () => true
  });
  assert.throws(
    () => replacedSource.begin('actor-1', 1n),
    /source ObjectGeneration changed from 1 to 2/u
  );
  assert.equal(replacedSource.isActive('actor-1'), false);

  const missingSource = new framework.ZLinkActorHandoffCoordinator({
    routedTransport: { async sendToSpot() {}, async requestToSpot() {} },
    requestSource() {
      throw new Error('source fence unavailable');
    },
    validateReplySource: () => true
  });
  assert.throws(
    () => missingSource.begin('actor-1', 1n),
    /source fence unavailable/
  );

  const { coordinator } = harness();
  coordinator.begin('actor-1', 1n);
  coordinator.snapshot('actor-1');
  const incompleteTarget = {
    ...target(),
    targetOwnerId: undefined,
    ownerLeaseGeneration: undefined
  };
  assert.throws(
    () => coordinator.complete('actor-1', incompleteTarget, targetActorRef(), []),
    /committed target owner fence/
  );
  assert.equal(coordinator.isActive('actor-1'), true);
  coordinator.cancel('actor-1');
});

test('in-flight handoff preserves moving packet arrival order in the commit backlog', async () => {
  const { coordinator, markers } = harness();
  coordinator.begin('actor-1', 1n);
  for (const value of ['P1', 'P2', 'P3']) {
    const parts = frame(value);
    await coordinator.capture('actor-1', parts, false, undefined, actorRef());
    parts.forEach((part) => part.close());
  }

  const backlog = coordinator.snapshot('actor-1');
  assert.deepEqual(backlog.map((packet) => Buffer.from(packet.payload, 'base64').toString()), ['P1', 'P2', 'P3']);
  assert.deepEqual(backlog.map((packet) => packet.index), [0, 1, 2]);
  assert.deepEqual(markers.map((entry) => entry.marker), [
    'handoff_backlog',
    'handoff_backlog',
    'handoff_backlog'
  ]);
});

test('provisional Join ingress replays one-way packets through the source mailbox in order', async () => {
  const { coordinator } = harness();
  const replayed = [];
  const replay = async (parts) => {
      replayed.push(parts[1].data().toString());
  };
  coordinator.beginProvisional('actor-1', 'join-1', 1n, 'source-node');
  for (const value of ['P1', 'P2']) {
    const parts = frame(value);
    await coordinator.capture(
      'actor-1',
      parts,
      false,
      undefined,
      actorRef(),
      undefined,
      undefined,
      replay
    );
    parts.forEach((part) => part.close());
  }

  await coordinator.releaseDeferred('actor-1', 'join-1');
  assert.deepEqual(replayed, ['P1', 'P2']);
  assert.equal(coordinator.isActive('actor-1'), false);
});

test('rejected provisional Join releases its current Actor mailbox record before source replay', async () => {
  const { coordinator } = harness();
  const actorMailbox = new ZLinkActorSerialExecutor('actor-1', 'source-spot');
  const spotSerial = new ZLinkSpotSerialTurnExecutor(true, 'source-spot');
  const events = [];
  const detachedErrors = [];
  let applicationAdmissions = 0;
  let activeApplicationAdmissions = 0;
  let peakApplicationAdmissions = 0;
  let newerQueued;
  let newerParts;
  let replayedNewerResolve;
  const replayedNewer = new Promise(resolve => {
    replayedNewerResolve = resolve;
  });
  const directReplay = (
    replayedParts,
    _returnResponse,
    _remoteBoundSessionTarget,
    _fallbackActorRef
  ) => actorMailbox.execute(() => spotSerial.execute(() => {
    const value = replayedParts[1].data().toString();
    events.push(`source:replayed:${value}`);
    if (value === 'newer') replayedNewerResolve();
  }));
  const replayInCurrentActorTurn = (
    replayedParts,
    _returnResponse,
    _remoteBoundSessionTarget,
    _fallbackActorRef
  ) => spotSerial.execute(() => {
    const value = replayedParts[1].data().toString();
    events.push(`source:replayed:${value}`);
    if (value === 'newer') replayedNewerResolve();
  });
  const transfer = new ZLinkActorTransferRuntime({
    actorHandoff: coordinator,
    spotManager: () => ({
      admitRoutedActorPacketPrefix(_spotId, _actorId, records) {
        const terminals = actorMailbox.admitDurablePrefix(
          records.map(record => ({
            operation: executeChild => record.drain((...args) =>
              executeChild(() => replayInCurrentActorTurn(...args))),
            preparation: record.preparation,
            workOptions: { payloadBytes: record.payloadBytes }
          }))
        );
        return { terminal: Promise.all(terminals).then(() => undefined) };
      },
      dispatchRoutedActorPacket(
        _spotId,
        actorId,
        replayedParts,
        returnResponse,
        remoteBoundSessionTarget,
        fallbackActorRef
      ) {
        return coordinator.capture(
          actorId,
          replayedParts,
          returnResponse,
          remoteBoundSessionTarget,
          fallbackActorRef,
          undefined,
          undefined,
          directReplay
        ) ?? directReplay(
          replayedParts,
          returnResponse,
          remoteBoundSessionTarget,
          fallbackActorRef
        );
      }
    }),
    reportPostCommitError(error) {
      detachedErrors.push(error);
    },
    async prepareApplicationJob() {
      applicationAdmissions += 1;
      activeApplicationAdmissions += 1;
      peakApplicationAdmissions = Math.max(
        peakApplicationAdmissions,
        activeApplicationAdmissions
      );
      let ready = true;
      return {
        async run(operation) {
          assert.equal(ready, true);
          ready = false;
          try {
            return await operation();
          } finally {
            activeApplicationAdmissions -= 1;
          }
        },
        cancel() {
          if (!ready) return;
          ready = false;
          activeApplicationAdmissions -= 1;
        }
      };
    }
  });
  try {
    const current = actorMailbox.execute(() => spotSerial.execute(async () => {
      events.push('handler');
      coordinator.beginProvisional('actor-1', 'join-rejected', 1n);
      for (const value of ['held-1', 'held-2']) {
        const parts = frame(value);
        try {
          await coordinator.capture(
            'actor-1',
            parts,
            false,
            undefined,
            actorRef(),
            undefined,
            undefined,
            directReplay
          );
        } finally {
          parts.forEach((part) => part.close());
        }
      }
      events.push('release:start');
      await transfer.cancelDeferredActorHandoff(
        { context: { actorId: 'actor-1' } },
        { spotId: 'source-spot' },
        'join-rejected'
      );
      events.push('release:end');
      newerParts = frame('newer');
      const captured = coordinator.capture(
        'actor-1',
        newerParts,
        false,
        undefined,
        actorRef(),
        undefined,
        undefined,
        directReplay
      );
      newerQueued = captured ?? directReplay(
        newerParts,
        false,
        undefined,
        actorRef()
      );
    }));

    let timeout;
    try {
      await Promise.race([
        current,
        new Promise((_, reject) => {
          timeout = setTimeout(
            () => reject(new Error(
              `rejected Join replay deadlocked behind its current Actor mailbox record: ${events.join(',')}`
            )),
            1_000
          );
        })
      ]);
    } finally {
      clearTimeout(timeout);
    }
    let replayTimeout;
    try {
      await Promise.race([
        Promise.all([newerQueued, replayedNewer]),
        new Promise((_, reject) => {
          replayTimeout = setTimeout(
            () => reject(new Error(`replayed backlog did not drain: ${events.join(',')}`)),
            1_000
          );
        })
      ]);
    } finally {
      clearTimeout(replayTimeout);
    }
    await actorMailbox.execute(() => spotSerial.execute(() => {
      events.push('actor:next');
    }));

    assert.deepEqual(events, [
      'handler',
      'release:start',
      'release:end',
      'source:replayed:held-1',
      'source:replayed:held-2',
      'source:replayed:newer',
      'actor:next'
    ]);
    assert.deepEqual(detachedErrors, []);
    assert.equal(applicationAdmissions, 3);
    assert.equal(peakApplicationAdmissions, 1);
    assert.equal(coordinator.isActive('actor-1'), false);
  } finally {
    newerParts?.forEach((part) => part.close());
    await actorMailbox.close();
    await spotSerial.close();
  }
});

test('provisional Join ingress preserves request completion when replayed locally', async () => {
  const { coordinator } = harness();
  const parts = frame('request');
  const ref = contextRef(parts, { request: true });
  coordinator.beginProvisional('actor-1', 'join-2', 1n, 'source-node');
  const pending = coordinator.capture(
    'actor-1',
    parts,
    true,
    undefined,
    ref,
    undefined,
    undefined,
    async (_parts, returnResponse) => returnResponse ? 'local-reply' : undefined
  );
  parts.forEach((part) => part.close());

  await coordinator.releaseDeferred('actor-1', 'join-2');
  assert.equal(await pending, 'local-reply');
});

test('deferred Join release uses the direct Spot replay path without recapturing itself', async () => {
  const { coordinator } = harness();
  const replayed = [];
  const reported = [];
  let ordinaryDispatches = 0;
  let directDispatches = 0;
  const transfer = new ZLinkActorTransferRuntime({
    actorHandoff: coordinator,
    spotManager: () => ({
      admitRoutedActorPacketPrefix(_spotId, _actorId, records) {
        const terminals = records.map(record => record.drain(async (
          parts,
          returnResponse,
          remoteBoundSessionTarget,
          fallbackActorRef
        ) => {
          directDispatches += 1;
          const value = parts[1].data().toString();
          replayed.push(value);
          if (value === 'D2') throw new Error('observed direct replay failure');
          return returnResponse
            ? { remoteBoundSessionTarget, fallbackActorRef }
            : undefined;
        }));
        return { terminal: Promise.allSettled(terminals).then(outcomes => {
          const failed = outcomes.find(outcome => outcome.status === 'rejected');
          if (failed !== undefined) throw failed.reason;
        }) };
      },
      dispatchRoutedActorPacket() {
        ordinaryDispatches += 1;
        throw new Error('release replay re-entered ordinary handoff capture');
      },
      async dispatchRoutedActorPacketDirect(
        _spotId,
        _actorId,
        parts
      ) {
        directDispatches += 1;
        const value = parts[1].data().toString();
        replayed.push(value);
        if (value === 'D2') throw new Error('observed direct replay failure');
      }
    }),
    reportPostCommitError(error) {
      reported.push(error);
    }
  });
  coordinator.beginProvisional('actor-1', 'join-direct', 1n, 'source-node');
  for (const value of ['D1', 'D2']) {
    const parts = frame(value);
    await coordinator.capture(
      'actor-1',
      parts,
      false,
      undefined,
      actorRef()
    );
    parts.forEach(part => part.close());
  }

  await transfer.completeDeferredActorHandoff(
    { context: { actorId: 'actor-1' } },
    { spotId: 'target-spot' },
    actorRef(),
    'join-direct'
  );
  await new Promise(resolve => setImmediate(resolve));

  assert.deepEqual(replayed, ['D1', 'D2']);
  assert.equal(directDispatches, 2);
  assert.equal(ordinaryDispatches, 0);
  assert.equal(reported.length, 1);
  assert.match(String(reported[0]), /observed direct replay failure/);
  assert.equal(coordinator.isActive('actor-1'), false);
});

test('release replay preserves typed request failures and observes one-way failures', async () => {
  const { coordinator } = harness();
  const requestDeadlineUnixMs = Date.now() + 10;
  const requestParts = frame('expired-release');
  coordinator.beginProvisional('actor-1', 'join-expired-release', 1n, 'source-node');
  const request = coordinator.capture(
    'actor-1',
    requestParts,
    true,
    undefined,
    contextRef(requestParts, {
      request: true,
      deadlineUnixMs: requestDeadlineUnixMs
    }),
    requestDeadlineUnixMs,
    undefined,
    async () => 'too-late'
  );
  requestParts.forEach(part => part.close());
  await new Promise(resolve => setTimeout(resolve, 20));
  await coordinator.releaseDeferred('actor-1', 'join-expired-release');
  await assert.rejects(
    request,
    error => error.kind === framework.ZLinkFrameworkErrorKind.DeadlineExceeded
  );

  coordinator.beginProvisional('actor-1', 'join-one-way-failure', 1n, 'source-node');
  const sendParts = frame('failed-release-send');
  await coordinator.capture(
    'actor-1',
    sendParts,
    false,
    undefined,
    actorRef(),
    undefined,
    undefined,
    async () => {
      throw new Error('one-way replay failed');
    }
  );
  sendParts.forEach(part => part.close());
  await assert.rejects(
    coordinator.releaseDeferred('actor-1', 'join-one-way-failure'),
    /one-way replay failed/
  );
  assert.equal(coordinator.isActive('actor-1'), false);
});

test('durable request handoff returns its initial permit before capacity-one replay admission', async () => {
  const { coordinator } = harness();
  const queue = new ApplicationJobQueue(
    resolveApplicationJobQueueConfiguration(
      { maxQueuedApplicationJobs: 1n },
      () => 1n
    )
  );
  coordinator.beginProvisional('actor-1', 'join-capacity-one', 1n, 'source-node');
  const parts = frame('capacity-one-request');
  const initialPermit = await queue.acquire();
  initialPermit.markApplicationQueued();
  const ingress = runWithApplicationJobPermit(initialPermit, () =>
    coordinator.capture(
      'actor-1',
      parts,
      true,
      undefined,
      actorRef(),
      undefined,
      undefined,
      async () => 'capacity-one-reply'
    )
  );
  parts.forEach(part => part.close());

  let replayAdmissions = 0;
  const release = coordinator.releaseDeferred(
    'actor-1',
    'join-capacity-one',
    undefined,
    async operation => {
      const permit = await queue.acquire();
      permit.markApplicationQueued();
      replayAdmissions += 1;
      return await runWithApplicationJobPermit(permit, operation);
    }
  );
  let timeout;
  try {
    const [, reply] = await Promise.race([
      Promise.all([release, ingress]),
      new Promise((_, reject) => {
        timeout = setTimeout(
          () => reject(new Error('capacity-one handoff replay did not reacquire its permit')),
          1_000
        );
      })
    ]);
    assert.equal(reply, 'capacity-one-reply');
  } finally {
    clearTimeout(timeout);
  }
  assert.equal(replayAdmissions, 1);
  assert.equal(queue.snapshot().permitsInUse, 0n);
  assert.equal(coordinator.isActive('actor-1'), false);
});

test('Message Follow preserves operation identity and rejects an exhausted hop with its marker', async () => {
  const { coordinator, messageFollowPayloads, markers } = harness();
  coordinator.begin('actor-1', 1n);
  coordinator.snapshot('actor-1');
  coordinator.complete(
    'actor-1',
    target(),
    targetActorRef(),
    [],
    ownerFence('target', 2n)
  );
  const parts = frame('last-hop');
  const visited = Array.from(
    { length: 7 },
    (_, index) => messageFollow.messageFollowOwnerFenceKey(
      ownerFence(`visited-${index}`, BigInt(index + 10))
    )
  );
  visited.push(messageFollow.messageFollowOwnerFenceKey(sourceOwnerFence()));
  const ref = contextRef(parts, {
    operationId: '77777777777777777777777777777777',
    hopCount: 7,
    visitedOwners: visited
  });
  await coordinator.capture('actor-1', parts, false, undefined, ref);
  parts.forEach((part) => part.close());
  assert.equal(
    messageFollowPayloads[0].messageFollowContext.operationId,
    '77777777777777777777777777777777'
  );
  assert.equal(messageFollowPayloads[0].messageFollowContext.hopCount, 8);

  const loop = frame('loop');
  const exhaustedVisited = Array.from(
    { length: 8 },
    (_, index) => messageFollow.messageFollowOwnerFenceKey(
      ownerFence(`exhausted-${index}`, BigInt(index + 30))
    )
  );
  exhaustedVisited.push(messageFollow.messageFollowOwnerFenceKey(sourceOwnerFence()));
  const exhausted = contextRef(loop, {
    operationId: '88888888888888888888888888888888',
    hopCount: 8,
    visitedOwners: exhaustedVisited
  });
  await assert.rejects(
    coordinator.capture('actor-1', loop, false, undefined, exhausted),
    (error) => error.kind === framework.ZLinkFrameworkErrorKind.Unavailable
  );
  loop.forEach((part) => part.close());
  assert.equal(
    markers.some((entry) => entry.marker === 'message_follow_rejected'),
    true
  );
});

test('Message Follow rejects repeated stale packets that do not carry the original immutable context', async () => {
  const { coordinator, followed, markers } = harness();
  coordinator.begin('actor-1', 1n);
  coordinator.snapshot('actor-1');
  coordinator.complete(
    'actor-1',
    target(),
    targetActorRef(),
    [],
    ownerFence('target', 2n)
  );

  for (let attempt = 0; attempt < 2; attempt++) {
    const parts = frame('missing-context');
    const departedRef = {
      ...actorRef(1n),
      nodeRid: zlink.RoutingId.from('source-node')
    };
    await assert.rejects(
      coordinator.capture('actor-1', parts, false, undefined, departedRef),
      (error) => error.kind === framework.ZLinkFrameworkErrorKind.Unavailable
    );
    parts.forEach((part) => part.close());
  }

  assert.deepEqual(followed, []);
  assert.equal(
    markers.filter((entry) => entry.marker === 'message_follow_rejected').length,
    2
  );
});

test('Message Follow request preserves its absolute deadline and drops a late relay reply', async () => {
  const requests = [];
  let completeRelay;
  const coordinator = new framework.ZLinkActorHandoffCoordinator({
    routedTransport: {
      async sendToSpot() {},
      async requestToSpot(_target, payload, options) {
        requests.push({ payload, options });
        return await new Promise((resolve) => { completeRelay = resolve; });
      }
    },
    messageFollowDurationMs: 1_000,
    requestTimeoutMs: 30_000,
    requestSource: () => ({
      meshName: 'mesh',
      objectGeneration: 1n,
      ownerId: 'source-owner',
      ownerLeaseGeneration: 1n,
      nodeRid: 'source-node',
      nodeGeneration: 1n,
      authorityOwnerGeneration: 1n
    }),
    validateReplySource: () => true
  });
  coordinator.begin('actor-1', 1n);
  coordinator.snapshot('actor-1');
  coordinator.complete('actor-1', target(), targetActorRef(), [], ownerFence('target', 2n));

  const deadlineUnixMs = Date.now() + 80;
  const correlationId = 'dddddddddddddddddddddddddddddddd';
  const requestHeader = streamProtocol.encodeStreamHeader({
    kind: streamProtocol.ZLinkStreamMessageKind.Request,
    codec: streamProtocol.ZLinkStreamCodec.Json,
    flags: streamProtocol.ZLinkStreamHeaderFlags.HasRequestSeq
      | streamProtocol.ZLinkStreamHeaderFlags.HasMetadata
      | streamProtocol.ZLinkStreamHeaderFlags.HasCorrelationId,
    requestSeq: 19n,
    name: 'DeadlineRequest',
    metadata: streamProtocol.actorRequestDeadlineMetadata(deadlineUnixMs),
    correlationId
  });
  const parts = [
    zlink.Message.from(Buffer.from(requestHeader)),
    zlink.Message.from(Buffer.from(JSON.stringify({ marker: 'deadline' })))
  ];
  const reply = coordinator.capture(
    'actor-1',
    parts,
    true,
    undefined,
    contextRef(parts, {
      request: true,
      deadlineUnixMs,
      correlationId
    })
  );
  parts.forEach((part) => part.close());

  await assert.rejects(
    reply,
    (error) => error.kind === framework.ZLinkFrameworkErrorKind.DeadlineExceeded
  );
  assert.equal(requests.length, 1);
  assert.equal(
    requests[0].payload.messageFollowContext.deadlineUnixMs,
    deadlineUnixMs
  );
  const relayedHeader = streamProtocol.decodeStreamHeader(
    Buffer.from(requests[0].payload.header, 'base64')
  );
  assert.equal(relayedHeader.correlationId, correlationId);
  assert.ok(requests[0].options.timeoutMs > 0);
  assert.ok(requests[0].options.timeoutMs <= 80);

  completeRelay({ ok: true, response: 'late' });
  await new Promise((resolve) => setImmediate(resolve));
});

test('Message Follow rejects an expired request before target transport admission', async () => {
  let requests = 0;
  const coordinator = new framework.ZLinkActorHandoffCoordinator({
    routedTransport: {
      async sendToSpot() {},
      async requestToSpot() {
        requests += 1;
        return { ok: true };
      }
    },
    messageFollowDurationMs: 1_000,
    requestSource: () => ({
      meshName: 'mesh',
      objectGeneration: 1n,
      ownerId: 'source-owner',
      ownerLeaseGeneration: 1n,
      nodeRid: 'source-node',
      nodeGeneration: 1n,
      authorityOwnerGeneration: 1n
    }),
    validateReplySource: () => true
  });
  coordinator.begin('actor-1', 1n);
  coordinator.snapshot('actor-1');
  coordinator.complete('actor-1', target(), targetActorRef(), [], ownerFence('target', 2n));

  const parts = frame('expired');
  const deadlineUnixMs = Date.now() - 1;
  await assert.rejects(
    coordinator.capture(
      'actor-1',
      parts,
      true,
      undefined,
      contextRef(parts, { request: true, deadlineUnixMs }),
      deadlineUnixMs
    ),
    (error) => error.kind === framework.ZLinkFrameworkErrorKind.DeadlineExceeded
  );
  parts.forEach((part) => part.close());
  assert.equal(requests, 0);
});

test('accepted handoff rejects an expired request before replay queue admission', async () => {
  const { coordinator } = harness();
  coordinator.begin('actor-1', 1n);
  const parts = frame('expired-accepted');
  const pending = coordinator.capture(
    'actor-1',
    parts,
    true,
    undefined,
    actorRef(1n),
    Date.now() - 1
  );
  parts.forEach((part) => part.close());
  await assert.rejects(
    pending,
    (error) => error.kind === framework.ZLinkFrameworkErrorKind.DeadlineExceeded
  );
  const backlog = coordinator.snapshot('actor-1');
  assert.equal(backlog.length, 0);
  coordinator.cancel('actor-1');
});

test('Message Follow route has no relocation-specific 1024-message admission cap', async () => {
  const markers = [];
  const coordinator = new framework.ZLinkActorHandoffCoordinator({
    routedTransport: { sendToSpot: async () => new Promise(() => {}) },
    messageFollowDurationMs: 60_000,
    requestSource: () => ({
      meshName: 'mesh',
      objectGeneration: 1n,
      ownerId: 'source-owner',
      ownerLeaseGeneration: 1n,
      nodeRid: 'source-node',
      nodeGeneration: 1n,
      authorityOwnerGeneration: 1n
    }),
    validateReplySource: () => true,
    onMarker: (marker, actorId, index) => markers.push({ marker, actorId, index })
  });
  coordinator.begin('actor-1', 1n);
  coordinator.snapshot('actor-1');
  coordinator.complete('actor-1', target(), targetActorRef(), [], ownerFence('target', 2n));
  for (let index = 0; index < 1024; index++) {
    const parts = frame(`queued-${index}`);
    void coordinator.capture(
      'actor-1',
      parts,
      false,
      undefined,
      contextRef(parts, {
        operationId: (index + 1).toString(16).padStart(32, '0')
      })
    );
    parts.forEach((part) => part.close());
  }
  const beyondFormerCap = frame('beyond-former-cap');
  const accepted = coordinator.capture(
    'actor-1',
    beyondFormerCap,
    false,
    undefined,
    contextRef(beyondFormerCap, { operationId: 'ffffffffffffffffffffffffffffffff' })
  );
  beyondFormerCap.forEach((part) => part.close());
  assert.equal(accepted instanceof Promise, true);
  assert.equal(
    markers.some((entry) => entry.marker === 'message_follow_rejected'),
    false
  );
});

test('Message Follow rejects an ActorRef generation mismatch as InvalidOperation', async () => {
  const { coordinator } = harness();
  coordinator.begin('actor-1', 1n);
  const parts = frame('generation-mismatch');
  const mismatched = contextRef(parts, {
    objectGeneration: 2n,
    actorRef: actorRef(1n)
  });

  await assert.rejects(
    coordinator.capture('actor-1', parts, false, undefined, mismatched),
    (error) => error.kind === framework.ZLinkFrameworkErrorKind.InvalidOperation
  );
  parts.forEach((part) => part.close());
  coordinator.cancel('actor-1');
});

test('packets captured after the commit snapshot use Message Follow after backlog completion', async () => {
  const { coordinator, followed } = harness();
  coordinator.begin('actor-1', 1n);
  const backlogParts = frame('B1');
  await coordinator.capture('actor-1', backlogParts, false, undefined, actorRef());
  backlogParts.forEach((part) => part.close());
  const backlog = coordinator.snapshot('actor-1');

  const directParts = frame('D1');
  await coordinator.capture('actor-1', directParts, false, undefined, actorRef());
  directParts.forEach((part) => part.close());
  assert.deepEqual(followed, []);

  coordinator.complete(
    'actor-1',
    target(),
    targetActorRef(),
    backlog.map((packet) => ({ index: packet.index, ok: true })),
    ownerFence('target', 2n)
  );
  await new Promise((resolve) => setImmediate(resolve));
  assert.deepEqual(followed, ['D1']);
});

test('bound-session packets keep one sequence across snapshot and Message Follow activation', async () => {
  const { coordinator, followed } = harness();
  const sessionTarget = {
    routerChannelId: 'session-mesh',
    targetNodeRid: zlink.RoutingId.from('session-node'),
    spotId: zlink.RoutingId.from('session-spot')
  };
  coordinator.begin('actor-1', 1n);
  for (const value of ['S1', 'S2']) {
    const parts = frame(value);
    await coordinator.capture('actor-1', parts, false, sessionTarget, actorRef());
    parts.forEach((part) => part.close());
  }
  const backlog = coordinator.snapshot('actor-1');
  const s3 = frame('S3');
  await coordinator.capture('actor-1', s3, false, sessionTarget, actorRef());
  s3.forEach((part) => part.close());
  coordinator.complete(
    'actor-1',
    target(),
    targetActorRef(),
    backlog.map((packet) => ({ index: packet.index, ok: true })),
    ownerFence('target', 2n)
  );
  const s4 = frame('S4');
  await coordinator.capture(
    'actor-1',
    s4,
    false,
    sessionTarget,
    contextRef(s4)
  );
  s4.forEach((part) => part.close());
  await new Promise((resolve) => setImmediate(resolve));

  assert.deepEqual(
    backlog.map((packet) => Buffer.from(packet.payload, 'base64').toString()).concat(followed),
    ['S1', 'S2', 'S3', 'S4']
  );
  assert.equal(backlog[0].remoteBoundSessionTarget.routerChannelId, 'session-mesh');
});

test('precommit abort returns the seal-era ingress hold to the source queue in order', async () => {
  const { coordinator } = harness();
  const replayed = [];
  coordinator.begin('actor-1', 1n);
  for (const value of ['H1', 'H2', 'H3']) {
    const parts = frame(value);
    await coordinator.capture('actor-1', parts, false, undefined, actorRef());
    parts.forEach((part) => part.close());
  }

  await coordinator.releaseCanceled('actor-1', async (parts) => {
    replayed.push(parts[1].data().toString());
  });
  assert.deepEqual(replayed, ['H1', 'H2', 'H3']);
  assert.equal(coordinator.isActive('actor-1'), false);
});

test('precommit abort drains appended ingress once through a single release task', async () => {
  const { coordinator } = harness();
  const replayed = [];
  let admissions = 0;
  let activeAdmissions = 0;
  let peakAdmissions = 0;
  let unblockFirstAdmission;
  let firstAdmissionStartedResolve;
  const firstAdmissionStarted = new Promise(resolve => {
    firstAdmissionStartedResolve = resolve;
  });
  const firstAdmissionBlocked = new Promise(resolve => {
    unblockFirstAdmission = resolve;
  });
  const admission = async operation => {
    admissions += 1;
    activeAdmissions += 1;
    peakAdmissions = Math.max(peakAdmissions, activeAdmissions);
    try {
      if (admissions === 1) {
        firstAdmissionStartedResolve();
        await firstAdmissionBlocked;
      }
      return await operation();
    } finally {
      activeAdmissions -= 1;
    }
  };
  const replay = async (parts, returnResponse) => {
    const value = parts[1].data().toString();
    replayed.push(value);
    return returnResponse ? `reply:${value}` : undefined;
  };

  coordinator.begin('actor-1', 1n);
  const requestParts = frame('P1');
  const request = coordinator.capture(
    'actor-1',
    requestParts,
    true,
    undefined,
    actorRef()
  );
  requestParts.forEach(part => part.close());
  const secondParts = frame('P2');
  await coordinator.capture('actor-1', secondParts, false, undefined, actorRef());
  secondParts.forEach(part => part.close());

  const firstRelease = coordinator.releaseCanceled('actor-1', replay, admission);
  await firstAdmissionStarted;
  const newerParts = frame('newer');
  await coordinator.capture('actor-1', newerParts, false, undefined, actorRef());
  newerParts.forEach(part => part.close());
  const duplicateRelease = coordinator.releaseCanceled('actor-1', replay, admission);
  coordinator.cancel('actor-1');
  unblockFirstAdmission();

  await Promise.all([firstRelease, duplicateRelease]);
  assert.equal(await request, 'reply:P1');
  assert.deepEqual(replayed, ['P1', 'P2', 'newer']);
  assert.equal(admissions, 3);
  assert.equal(peakAdmissions, 1);
  assert.equal(coordinator.isActive('actor-1'), false);
});

test('a sealed Session route refuses a connection-bound send at capture and keeps it out of the backlog', async () => {
  const { coordinator } = harness();
  const sessionTarget = {
    routerChannelId: 'session-mesh',
    targetNodeRid: zlink.RoutingId.from('session-node'),
    spotId: zlink.RoutingId.from('session-spot')
  };
  coordinator.begin('actor-1', 1n);
  const preSeal = frame('S1');
  await coordinator.capture('actor-1', preSeal, false, sessionTarget, actorRef());
  preSeal.forEach((part) => part.close());

  coordinator.sealConnectionBoundIngress('actor-1');
  const postSeal = frame('S2');
  await assert.rejects(
    coordinator.capture('actor-1', postSeal, false, sessionTarget, actorRef()),
    (error) => error.kind === framework.ZLinkFrameworkErrorKind.Unavailable
  );
  postSeal.forEach((part) => part.close());

  const direct = frame('D1');
  await coordinator.capture('actor-1', direct, false, undefined, actorRef());
  direct.forEach((part) => part.close());

  const backlog = coordinator.snapshot('actor-1');
  assert.deepEqual(
    backlog.map((packet) => Buffer.from(packet.payload, 'base64').toString()),
    ['S1', 'D1']
  );
  coordinator.cancel('actor-1');
});

test('Message Follow relays before duration expiry and prunes route and stale records after removal', async () => {
  const { coordinator, followed, markers } = harness(10);
  coordinator.begin('actor-1', 1n);
  coordinator.snapshot('actor-1');
  coordinator.complete('actor-1', target(), targetActorRef(), [], ownerFence('target', 2n));

  const inside = frame('G1');
  await coordinator.capture('actor-1', inside, false, undefined, contextRef(inside));
  inside.forEach((part) => part.close());
  assert.deepEqual(followed, ['G1']);

  await new Promise((resolve) => setTimeout(resolve, 20));
  assert.equal(markers.some((entry) => entry.marker === 'message_follow_route_removed'), true);
  assert.equal(coordinator.messageFollowCount('actor-1'), 0);

  // The stale-tenure records live only as long as the follow window: a late
  // send no longer receives a terminal stale verdict here and returns to the
  // normal resolution path instead.
  const outside = frame('G2');
  assert.equal(
    coordinator.capture('actor-1', outside, false, undefined, contextRef(outside)),
    undefined
  );
  outside.forEach((part) => part.close());
  assert.deepEqual(followed, ['G1']);
  assert.equal(coordinator.isKnownStale(actorRef(1n)), false);
});

test('Message Follow notification suppression retries rejection and marks only accepted transport', async () => {
  let notificationAttempts = 0;
  let acceptNotification = false;
  const coordinator = new framework.ZLinkActorHandoffCoordinator({
    routedTransport: {
      async sendToSpot() {},
      async requestToSpot() { return { ok: true }; }
    },
    messageFollowDurationMs: 1_000,
    requestSource: () => ({
      meshName: 'mesh',
      objectGeneration: 1n,
      ownerId: 'source-owner',
      ownerLeaseGeneration: 1n,
      nodeRid: 'source-node',
      nodeGeneration: 1n,
      authorityOwnerGeneration: 1n
    }),
    validateReplySource: () => true,
    onMessageFollowRelayed: async () => {
      notificationAttempts += 1;
      return acceptNotification;
    }
  });
  coordinator.begin('actor-1', 1n);
  coordinator.snapshot('actor-1');
  coordinator.complete(
    'actor-1',
    target(),
    targetActorRef(),
    [],
    ownerFence('target', 2n)
  );
  const origin = {
    sourceNodeRid: zlink.RoutingId.from('ingress-source'),
    originalOperation: { high: 7n, low: 11n },
    originalReplyRouteId: 13n
  };

  const relay = async (value, operationId) => {
    const parts = frame(value);
    try {
      await coordinator.capture(
        'actor-1',
        parts,
        false,
        undefined,
        contextRef(parts, { operationId }),
        undefined,
        origin
      );
    } finally {
      parts.forEach((part) => part.close());
    }
  };

  await relay('retry-notification', '10101010101010101010101010101010');
  assert.equal(notificationAttempts, 1);
  acceptNotification = true;
  await relay('accepted-notification', '20202020202020202020202020202020');
  assert.equal(notificationAttempts, 2);
  await relay('suppressed-notification', '30303030303030303030303030303030');
  assert.equal(notificationAttempts, 2);
});

test('a returning tenure with a newer authority fence bypasses the departed stale record', async () => {
  const { coordinator, followed, markers, setRequestSource } = harness();
  setRequestSource({
    objectGeneration: 1n,
    ownerId: 'source-owner',
    ownerLeaseGeneration: 1n,
    nodeRid: 'source',
    nodeGeneration: 1n,
    authorityOwnerGeneration: 1n
  });
  coordinator.begin('actor-1', 1n, 'source');
  coordinator.snapshot('actor-1');
  coordinator.complete('actor-1', target(), targetActorRef(), [], ownerFence('target', 2n));

  // A→B→A: the relocation preserved the ObjectGeneration, so the returning
  // tenure re-uses the departed (nodeRid, objectGeneration) pair — only its
  // authority fence is strictly newer than the recorded departure.
  const returnedFence = messageFollow.ownerFence({
    ownerId: 'returned-owner',
    ownerLeaseGeneration: 2n,
    nodeRid: 'source',
    nodeGeneration: 2n,
    authorityOwnerGeneration: 3n
  });
  const returned = frame('returned-tenure');
  assert.equal(
    coordinator.capture('actor-1', returned, false, undefined, contextRef(returned, {
      sourceOwner: returnedFence,
      targetOwner: returnedFence
    })),
    undefined
  );
  returned.forEach((part) => part.close());
  assert.deepEqual(followed, []);
  assert.equal(markers.some((entry) => entry.marker === 'message_follow_rejected'), false);
  assert.equal(markers.some((entry) => entry.marker === 'message_follow_expired'), false);

  // A context that still addresses the departed tenure keeps following.
  const departedFence = messageFollow.ownerFence({
    ownerId: 'source-owner',
    ownerLeaseGeneration: 1n,
    nodeRid: 'source',
    nodeGeneration: 1n,
    authorityOwnerGeneration: 1n
  });
  const departed = frame('departed-tenure');
  await coordinator.capture('actor-1', departed, false, undefined, contextRef(departed, {
    sourceOwner: departedFence,
    targetOwner: departedFence
  }));
  departed.forEach((part) => part.close());
  assert.deepEqual(followed, ['departed-tenure']);
});

test('a Core-routed packet owned by the current actor bypasses an older Message Follow route', async () => {
  const { coordinator, followed, markers } = harness();
  coordinator.begin('actor-1', 1n);
  coordinator.snapshot('actor-1');
  coordinator.complete('actor-1', target(), targetActorRef(), [], ownerFence('target', 2n));

  const current = frame('current-owner');
  assert.equal(
    coordinator.capture('actor-1', current, false, undefined, actorRef(2n)),
    undefined
  );
  current.forEach((part) => part.close());

  assert.deepEqual(followed, []);
  assert.equal(markers.some((entry) => entry.marker === 'message_follow_relay'), false);
});

test('a current ActorRef bypasses an older same-generation route after returning to the node', async () => {
  const { coordinator, followed, markers, setCurrentGeneration, setCurrentNodeRid } = harness();
  coordinator.begin('actor-1', 1n);
  coordinator.snapshot('actor-1');
  coordinator.complete(
    'actor-1',
    target(),
    targetActorRef('target', 1n),
    [],
    ownerFence('target', 2n)
  );
  setCurrentGeneration(1n);
  setCurrentNodeRid(String(actorRef(1n).nodeRid));

  const current = frame('returned-owner');
  assert.equal(
    coordinator.capture('actor-1', current, false, undefined, actorRef(1n)),
    undefined
  );
  current.forEach((part) => part.close());

  assert.deepEqual(followed, []);
  assert.equal(markers.some((entry) => entry.marker === 'message_follow_relay'), false);
});

test('an exact current-owner context bypasses an older Message Follow route', async () => {
  const {
    coordinator,
    followed,
    markers,
    setCurrentGeneration
  } = harness();
  coordinator.begin('actor-1', 1n);
  coordinator.snapshot('actor-1');
  const currentFence = ownerFence('target', 2n);
  setCurrentGeneration(1n);
  coordinator.complete(
    'actor-1',
    target(),
    targetActorRef(),
    [],
    currentFence
  );

  const current = frame('current-target');
  const owner = contextRef(current, {
    sourceOwner: currentFence,
    targetOwner: currentFence,
    actorRef: targetActorRef()
  });
  assert.equal(
    coordinator.capture('actor-1', current, false, undefined, owner),
    undefined
  );
  current.forEach((part) => part.close());

  assert.deepEqual(followed, []);
  assert.equal(markers.some((entry) => entry.marker === 'message_follow_relay'), false);
});

test('chained relocation keeps exact source-owner routes with one ObjectGeneration', async () => {
  const { coordinator, followed, messageFollowPayloads, setRequestSource } = harness(10);
  coordinator.begin('actor-1', 1n);
  coordinator.snapshot('actor-1');
  const firstFence = ownerFence('first', 2n);
  coordinator.complete(
    'actor-1',
    target('first'),
    targetActorRef('first', 1n),
    [],
    firstFence
  );
  assert.equal(coordinator.messageFollowCount('actor-1'), 1);

  setRequestSource({
    objectGeneration: 1n,
    ownerId: firstFence.ownerId,
    ownerLeaseGeneration: BigInt(firstFence.ownerLeaseGeneration),
    nodeRid: firstFence.nodeRid,
    nodeGeneration: BigInt(firstFence.nodeGeneration),
    authorityOwnerGeneration: BigInt(firstFence.authorityOwnerGeneration)
  });
  coordinator.begin('actor-1', 1n, 'first-node', 2n, 1n);
  coordinator.snapshot('actor-1');
  coordinator.complete(
    'actor-1',
    target('second'),
    targetActorRef('second', 1n),
    [],
    ownerFence('second', 3n)
  );
  assert.equal(coordinator.messageFollowCount('actor-1'), 2);
  const packet = frame('chain');
  const sourceContext = contextRef(packet, {
    operationId: '44444444444444444444444444444444'
  });
  await coordinator.capture('actor-1', packet, false, undefined, sourceContext);
  const firstRelayContext = messageFollow.decodeActorMessageFollowContext(
    messageFollowPayloads[0].messageFollowContext
  );
  const firstTargetRef = messageFollow.attachActorMessageFollowContext(
    targetActorRef('first', 1n),
    firstRelayContext
  );
  await coordinator.capture(
    'actor-1',
    packet,
    false,
    undefined,
    firstTargetRef
  );
  packet.forEach((part) => part.close());
  assert.deepEqual(followed, ['chain', 'chain']);
  assert.equal(
    messageFollowPayloads[1].messageFollowContext.operationId,
    '44444444444444444444444444444444'
  );
  assert.equal(messageFollowPayloads[1].messageFollowContext.hopCount, 2);

  await new Promise((resolve) => setTimeout(resolve, 20));
  assert.equal(coordinator.messageFollowCount('actor-1'), 0);
});

test('duplicate Message Follow operation reaches the target exactly once', async () => {
  const { coordinator, followed } = harness(1_000);
  coordinator.begin('actor-1', 1n);
  coordinator.snapshot('actor-1');
  coordinator.complete(
    'actor-1',
    target(),
    targetActorRef(),
    [],
    ownerFence('target', 2n)
  );
  const first = frame('deduplicated');
  const second = frame('deduplicated');
  const firstRef = contextRef(first, {
    operationId: '55555555555555555555555555555555'
  });
  const secondRef = contextRef(second, {
    operationId: '55555555555555555555555555555555'
  });

  await Promise.all([
    coordinator.capture('actor-1', first, false, undefined, firstRef),
    coordinator.capture('actor-1', second, false, undefined, secondRef)
  ]);
  first.forEach((part) => part.close());
  second.forEach((part) => part.close());
  assert.deepEqual(followed, ['deduplicated']);
});

test('Message Follow rejects a visited target owner before transport admission', async () => {
  const { coordinator, followed, markers } = harness(1_000);
  coordinator.begin('actor-1', 1n);
  coordinator.snapshot('actor-1');
  const nextOwner = ownerFence('target', 2n);
  coordinator.complete('actor-1', target(), targetActorRef(), [], nextOwner);
  const parts = frame('loop');
  const source = sourceOwnerFence();
  const ref = contextRef(parts, {
    operationId: '66666666666666666666666666666666',
    sourceOwner: ownerFence('visited-origin', 9n),
    targetOwner: source,
    hopCount: 1,
    visitedOwners: [
      messageFollow.messageFollowOwnerFenceKey(nextOwner),
      messageFollow.messageFollowOwnerFenceKey(source)
    ]
  });
  await assert.rejects(
    coordinator.capture('actor-1', parts, false, undefined, ref),
    (error) => error.kind === framework.ZLinkFrameworkErrorKind.Unavailable
  );
  parts.forEach((part) => part.close());
  assert.deepEqual(followed, []);
  assert.equal(
    markers.some((entry) => entry.marker === 'message_follow_rejected'),
    true
  );
});

test('node-direct and spot-direct relay wire preserve the immutable context', () => {
  const parts = frame('wire');
  const ref = contextRef(parts, {
    operationId: '99999999999999999999999999999999',
    request: true,
    deadlineUnixMs: Date.now() + 10_000
  });
  const context = messageFollow.actorMessageFollowContext(ref);
  const nodeDirect = actorRelayWire.decodeRemoteActorPacketRelayPayload(
    actorRelayWire.encodeRemoteActorPacketRelayPayload({
      actorId: 'actor-1',
      header: parts[0].data(),
      payload: parts[1].data(),
      bindingActorRef: actorRef(1n),
      returnResponse: true,
      messageFollowContext: context
    })
  );
  const spotDirect = actorRelayWire.decodeRemoteActorPacketRelayPayload(
    actorRelayWire.encodeMessageFollowRemoteActorPacketRelayPayload({
      actorId: 'actor-1',
      header: Buffer.from(parts[0].data()).toString('base64'),
      payload: Buffer.from(parts[1].data()).toString('base64'),
      actorNodeRid: 'target-node',
      actorGeneration: '1',
      returnResponse: true,
      messageFollowContext: context
    })
  );
  parts.forEach((part) => part.close());

  for (const decoded of [nodeDirect, spotDirect]) {
    assert.equal(decoded.returnResponse, true);
    assert.deepEqual(decoded.messageFollowContext, context);
    assert.equal(Object.isFrozen(decoded.messageFollowContext), true);
    assert.equal(Object.isFrozen(decoded.messageFollowContext.visitedOwners), true);
  }
});

test('positive Message Follow request returns one correlated reply and preserves typed errors', async () => {
  const relays = [];
  let response = {
    ok: true,
    response: { accepted: true }
  };
  const coordinator = new framework.ZLinkActorHandoffCoordinator({
    routedTransport: {
      async sendToSpot() {},
      async requestToSpot(_target, payload) {
        relays.push(payload);
        return response;
      }
    },
    messageFollowDurationMs: 1_000,
    requestSource: () => ({
      meshName: 'mesh',
      objectGeneration: 1n,
      ownerId: 'source-owner',
      ownerLeaseGeneration: 1n,
      nodeRid: 'source-node',
      nodeGeneration: 1n,
      authorityOwnerGeneration: 1n
    }),
    validateReplySource: () => true
  });
  coordinator.begin('actor-1', 1n, 'source-node', 1n, 1n);
  coordinator.snapshot('actor-1');
  coordinator.complete(
    'actor-1',
    target(),
    targetActorRef(),
    [],
    ownerFence('target', 2n)
  );
  const request = frame('positive-request');
  const requestRef = contextRef(request, {
    operationId: 'aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa',
    request: true,
    correlationId: 'bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb',
    replyRouteId: 'cccccccccccccccccccccccccccccccc',
    deadlineUnixMs: Date.now() + 10_000
  });
  assert.deepEqual(
    await coordinator.capture('actor-1', request, true, undefined, requestRef),
    { accepted: true }
  );
  request.forEach((part) => part.close());
  assert.equal(relays[0].returnResponse, true);
  assert.equal(
    relays[0].messageFollowContext.correlationId,
    'bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb'
  );
  assert.equal(
    relays[0].messageFollowContext.replyRouteId,
    'cccccccccccccccccccccccccccccccc'
  );

  response = {
    ok: false,
    error: 'moving',
      errorKind: framework.ZLinkFrameworkInternalErrorKind.ActorMoving
  };
  const failed = frame('typed-error');
  const failedRef = contextRef(failed, {
    operationId: 'dddddddddddddddddddddddddddddddd',
    request: true,
    deadlineUnixMs: Date.now() + 10_000
  });
  await assert.rejects(
    coordinator.capture('actor-1', failed, true, undefined, failedRef),
    (error) => error.kind === framework.ZLinkFrameworkErrorKind.Unavailable
  );
  failed.forEach((part) => part.close());
});

test('in-flight request preserves framing, reply correlation, and the caller timeout', async () => {
  const { coordinator, setRequestSource } = harness();
  const requestHeader = streamProtocol.encodeStreamHeader({
    kind: streamProtocol.ZLinkStreamMessageKind.Request,
    codec: streamProtocol.ZLinkStreamCodec.Json,
    flags: streamProtocol.ZLinkStreamHeaderFlags.None,
    requestSeq: 731n,
    name: 'HandoffRequest',
    metadata: new Map([['trace-id', 'handoff-731']]),
    correlationId: 'caller-731'
  });
  const parts = [
    zlink.Message.from(Buffer.from(requestHeader)),
    zlink.Message.from(Buffer.from(JSON.stringify({ marker: 'R1' })))
  ];

  coordinator.begin('actor-1', 1n);
  const pendingReply = coordinator.capture('actor-1', parts, true, undefined, actorRef());
  parts.forEach((part) => part.close());
  const backlog = coordinator.snapshot('actor-1');
  const replayHeader = streamProtocol.decodeStreamHeader(Buffer.from(backlog[0].header, 'base64'));

  assert.equal(backlog[0].returnResponse, true);
  assert.equal(replayHeader.kind, streamProtocol.ZLinkStreamMessageKind.Request);
  assert.equal(replayHeader.requestSeq, 731n);
  assert.equal(replayHeader.correlationId, 'caller-731');
  assert.equal(replayHeader.metadata.get('trace-id'), 'handoff-731');
  assert.notEqual(replayHeader.flags & streamProtocol.ZLinkStreamHeaderFlags.HasRequestSeq, 0);
  assert.notEqual(replayHeader.flags & streamProtocol.ZLinkStreamHeaderFlags.HasMetadata, 0);

  const terminal = {
    index: backlog[0].index,
    ok: true,
    response: { marker: 'R1', requestSeq: '731' }
  };
  coordinator.complete(
    'actor-1',
    target(),
    targetActorRef(),
    [terminal],
    ownerFence('target', 2n)
  );
  assert.equal(
    coordinator.acceptRelocatedTerminal('actor-1', backlog[0], terminal, 'target-node', 2n),
    'terminalReceived'
  );
  assert.deepEqual(await pendingReply, { marker: 'R1', requestSeq: '731' });

  setRequestSource({
    objectGeneration: 2n,
    ownerId: 'source-owner',
    ownerLeaseGeneration: 1n,
    nodeRid: 'source-node',
    nodeGeneration: 1n,
    authorityOwnerGeneration: 1n
  });
  coordinator.begin('actor-1', 2n);
  const lateParts = [
    zlink.Message.from(Buffer.from(requestHeader)),
    zlink.Message.from(Buffer.from(JSON.stringify({ marker: 'late' })))
  ];
  const lateReply = coordinator.capture('actor-1', lateParts, true, undefined, actorRef(2n));
  lateParts.forEach((part) => part.close());
  const lateBacklog = coordinator.snapshot('actor-1');
  const caller = Promise.race([
    lateReply,
    new Promise((_resolve, reject) => setTimeout(() => reject(new Error('normal request timeout')), 5))
  ]);
  await assert.rejects(caller, /normal request timeout/);
  const lateTerminal = {
    index: lateBacklog[0].index,
    ok: true,
    response: { marker: 'late' }
  };
  coordinator.complete(
    'actor-1',
    target('late'),
    targetActorRef('late', 3n),
    [lateTerminal],
    ownerFence('late', 2n)
  );
  assert.equal(
    coordinator.acceptRelocatedTerminal(
      'actor-1', lateBacklog[0], lateTerminal, 'late-node', 2n
    ),
    'terminalReceived'
  );
  assert.deepEqual(await lateReply, { marker: 'late' });
});

test('late terminal uses captured source evidence after Actor removal and rejects every stale fence', async () => {
  const {
    coordinator,
    removeSourceState,
    sourceLookupCount,
    setReplyHostOwnerLeaseGeneration,
    setReplyHostNodeGeneration
  } = harness();
  const request = frame('captured-terminal');
  coordinator.begin('actor-1', 1n);
  const pendingReply = coordinator.capture('actor-1', request, true, undefined, actorRef());
  assert.equal(sourceLookupCount(), 1);
  request.forEach((part) => part.close());
  const [packet] = coordinator.snapshot('actor-1');
  const terminal = { index: packet.index, ok: true, response: { accepted: true } };
  coordinator.complete(
    'actor-1',
    target(),
    targetActorRef(),
    [terminal],
    ownerFence('target', 2n)
  );
  removeSourceState();

  const withSource = (source) => ({ ...packet, source: { ...packet.source, ...source } });
  assert.equal(
    coordinator.acceptRelocatedTerminal(
      'actor-1', withSource({ ownerId: 'forged-owner' }), terminal, 'target-node', 2n
    ),
    'notAcknowledged'
  );
  assert.equal(
    coordinator.acceptRelocatedTerminal(
      'actor-1', withSource({ ownerLeaseGeneration: '2' }), terminal, 'target-node', 2n
    ),
    'notAcknowledged'
  );
  assert.equal(
    coordinator.acceptRelocatedTerminal(
      'actor-1', withSource({ nodeRid: 'forged-node' }), terminal, 'target-node', 2n
    ),
    'notAcknowledged'
  );
  assert.equal(
    coordinator.acceptRelocatedTerminal(
      'actor-1', withSource({ nodeGeneration: '2' }), terminal, 'target-node', 2n
    ),
    'notAcknowledged'
  );
  assert.equal(
    coordinator.acceptRelocatedTerminal(
      'actor-1', withSource({ replyRouteId: 'ffffffffffffffffffffffffffffffff' }),
      terminal, 'target-node', 2n
    ),
    'notAcknowledged'
  );
  assert.equal(
    coordinator.acceptRelocatedTerminal(
      'forged-actor', packet, terminal, 'target-node', 2n
    ),
    'notAcknowledged'
  );
  assert.equal(
    coordinator.acceptRelocatedTerminal(
      'actor-1',
      {
        ...packet,
        messageFollowContext: {
          ...packet.messageFollowContext,
          operationId: 'ffffffffffffffffffffffffffffffff'
        }
      },
      terminal,
      'target-node',
      2n
    ),
    'notAcknowledged'
  );
  assert.equal(
    coordinator.acceptRelocatedTerminal('actor-1', packet, terminal, 'forged-target', 2n),
    'notAcknowledged'
  );
  assert.equal(
    coordinator.acceptRelocatedTerminal('actor-1', packet, terminal, 'target-node', 3n),
    'notAcknowledged'
  );
  assert.equal(
    coordinator.acceptRelocatedTerminal(
      'actor-1',
      {
        ...packet,
        messageFollowContext: {
          ...packet.messageFollowContext,
          objectGeneration: '2'
        }
      },
      terminal,
      'target-node',
      2n
    ),
    'notAcknowledged'
  );
  assert.equal(
    coordinator.acceptRelocatedTerminal(
      'actor-1',
      {
        ...packet,
        messageFollowContext: {
          ...packet.messageFollowContext,
          targetOwner: {
            ...packet.messageFollowContext.targetOwner,
            authorityOwnerGeneration: '2'
          }
        }
      },
      terminal,
      'target-node',
      2n
    ),
    'notAcknowledged'
  );

  setReplyHostOwnerLeaseGeneration(2n);
  assert.equal(
    coordinator.acceptRelocatedTerminal('actor-1', packet, terminal, 'target-node', 2n),
    'notAcknowledged'
  );
  setReplyHostOwnerLeaseGeneration(1n);
  setReplyHostNodeGeneration(2n);
  assert.equal(
    coordinator.acceptRelocatedTerminal('actor-1', packet, terminal, 'target-node', 2n),
    'notAcknowledged'
  );
  setReplyHostNodeGeneration(1n);

  assert.equal(
    coordinator.acceptRelocatedTerminal('actor-1', packet, terminal, 'target-node', 2n),
    'terminalReceived'
  );
  assert.equal(sourceLookupCount(), 1);
  assert.deepEqual(await pendingReply, { accepted: true });
});

test('Message Follow keeps opaque node identities that share the same display text', async () => {
  const sourceA = routingIds.decodeRoutingId('\ufffd', 'ff');
  const sourceB = routingIds.decodeRoutingId('\ufffd', 'fe');
  assert.equal(String(sourceA), String(sourceB));
  assert.equal(routingIds.routingIdsEqual(sourceA, sourceB), false);

  const textPrefixFence = messageFollow.ownerFence({
    ownerId: 'same-owner',
    ownerLeaseGeneration: 1n,
    nodeRid: 'rid:ff',
    nodeGeneration: 1n,
    authorityOwnerGeneration: 1n
  });
  const opaquePrefixFence = messageFollow.ownerFence({
    ownerId: 'same-owner',
    ownerLeaseGeneration: 1n,
    nodeRid: String(sourceA),
    nodeRidHex: routingIds.encodeRoutingIdStorageHex(sourceA),
    nodeGeneration: 1n,
    authorityOwnerGeneration: 1n
  });
  assert.notEqual(
    messageFollow.messageFollowOwnerFenceKey(textPrefixFence),
    messageFollow.messageFollowOwnerFenceKey(opaquePrefixFence)
  );
  assert.equal(
    messageFollow.messageFollowOwnerFencesEqual(textPrefixFence, opaquePrefixFence),
    false
  );

  const sourceFenceA = messageFollow.ownerFence({
    ownerId: 'same-owner',
    ownerLeaseGeneration: 1n,
    nodeRid: String(sourceA),
    nodeRidHex: routingIds.encodeRoutingIdStorageHex(sourceA),
    nodeGeneration: 1n,
    authorityOwnerGeneration: 1n
  });
  const sourceFenceB = messageFollow.ownerFence({
    ownerId: 'same-owner',
    ownerLeaseGeneration: 1n,
    nodeRid: String(sourceB),
    nodeRidHex: routingIds.encodeRoutingIdStorageHex(sourceB),
    nodeGeneration: 1n,
    authorityOwnerGeneration: 1n
  });
  assert.notEqual(
    messageFollow.messageFollowOwnerFenceKey(sourceFenceA),
    messageFollow.messageFollowOwnerFenceKey(sourceFenceB)
  );
  assert.equal(messageFollow.messageFollowOwnerFencesEqual(sourceFenceA, sourceFenceB), false);

  let source = {
    meshName: 'mesh',
    objectGeneration: 1n,
    ownerId: 'same-owner',
    ownerLeaseGeneration: 1n,
    nodeRid: String(sourceA),
    nodeRidHex: routingIds.encodeRoutingIdStorageHex(sourceA),
    nodeGeneration: 1n,
    authorityOwnerGeneration: 1n
  };
  const relays = [];
  const coordinator = new framework.ZLinkActorHandoffCoordinator({
    routedTransport: {
      async sendToSpot(targetRoute, payload) {
        relays.push({ targetNodeRid: String(targetRoute.targetNodeRid), payload });
      },
      async requestToSpot() { return { ok: true }; }
    },
    requestSource: () => source,
    validateReplySource: () => true
  });

  coordinator.begin('actor-1', 1n);
  coordinator.snapshot('actor-1');
  coordinator.complete(
    'actor-1', target('first'), targetActorRef('first'), [], ownerFence('first', 2n)
  );
  source = {
    ...source,
    nodeRid: String(sourceB),
    nodeRidHex: routingIds.encodeRoutingIdStorageHex(sourceB)
  };
  coordinator.begin('actor-1', 1n);
  coordinator.snapshot('actor-1');
  coordinator.complete(
    'actor-1', target('second'), targetActorRef('second'), [], ownerFence('second', 2n)
  );
  assert.equal(coordinator.messageFollowCount('actor-1'), 2);

  for (const [sourceRid, sourceOwner] of [[sourceA, sourceFenceA], [sourceB, sourceFenceB]]) {
    const parts = frame(`opaque-${sourceOwner.nodeRidHex}`);
    const ref = contextRef(parts, {
      actorRef: { ...actorRef(), nodeRid: sourceRid },
      sourceOwner,
      targetOwner: sourceOwner
    });
    await coordinator.capture('actor-1', parts, false, undefined, ref);
    parts.forEach((part) => part.close());
  }
  assert.deepEqual(relays.map((relay) => relay.targetNodeRid), ['first-node', 'second-node']);
  assert.deepEqual(
    relays.map((relay) => relay.payload.actorNodeRidHex),
    [
      Buffer.from('first-node').toString('hex'),
      Buffer.from('second-node').toString('hex')
    ]
  );
  assert.deepEqual(
    relays.map((relay) =>
      actorRelayWire.decodeRemoteActorPacketRelayPayload(
        JSON.parse(JSON.stringify(relay.payload))
      ).messageFollowContext.sourceOwner.nodeRidHex),
    ['ff', 'fe']
  );

  coordinator.begin('actor-1', 1n);
  const terminalParts = frame('opaque-target-terminal');
  const terminalReply = coordinator.capture(
    'actor-1', terminalParts, true, undefined, { ...actorRef(), nodeRid: sourceB }
  );
  terminalParts.forEach((part) => part.close());
  const [terminalPacket] = coordinator.snapshot('actor-1');
  const terminal = { index: terminalPacket.index, ok: true, response: 'opaque-target' };
  const opaqueTarget = {
    ...target('opaque'),
    targetNodeRid: sourceA
  };
  const opaqueTargetRef = {
    ...targetActorRef('opaque'),
    nodeRid: sourceA
  };
  const opaqueTargetOwner = messageFollow.ownerFence({
    ownerId: 'opaque-owner',
    ownerLeaseGeneration: 1n,
    nodeRid: String(sourceA),
    nodeRidHex: routingIds.encodeRoutingIdStorageHex(sourceA),
    nodeGeneration: 1n,
    authorityOwnerGeneration: 2n
  });
  coordinator.complete(
    'actor-1', opaqueTarget, opaqueTargetRef, [terminal], opaqueTargetOwner
  );
  assert.equal(
    coordinator.acceptRelocatedTerminal(
      'actor-1', terminalPacket, terminal, sourceB, 2n
    ),
    'notAcknowledged'
  );
  assert.equal(
    coordinator.acceptRelocatedTerminal(
      'actor-1', terminalPacket, terminal, sourceA, 2n
    ),
    'terminalReceived'
  );
  assert.equal(await terminalReply, 'opaque-target');
  const duplicate = coordinator.acceptRelocatedTerminalRelay(
    terminalPacket.messageFollowContext.operationId,
    terminalPacket.source.replyRouteId,
    undefined,
    terminal,
    sourceA,
    2n,
    'actor-1'
  );
  assert.equal(duplicate.status, 'alreadyTerminal');
  assert.equal(routingIds.routingIdsEqual(duplicate.source.nodeRid, sourceB), true);
  assert.equal(routingIds.routingIdsEqual(duplicate.source.nodeRid, sourceA), false);
});

test('Message Follow accepts legacy visited owner keys without weakening opaque RID fences', () => {
  const parts = frame('legacy-visited-owner');
  const legacyKey = (fence) => [
    fence.nodeRid,
    fence.nodeGeneration,
    fence.ownerId,
    fence.ownerLeaseGeneration,
    fence.authorityOwnerGeneration
  ].join('\u0000');
  const legacyOwner = sourceOwnerFence();
  const legacyOwnerKey = legacyKey(legacyOwner);
  const legacyContext = {
    operationId: 'aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa',
    objectGeneration: '1',
    sourceOwner: legacyOwner,
    targetOwner: legacyOwner,
    request: false,
    hopCount: 0,
    visitedOwners: [legacyOwnerKey],
    payloadChecksumSha256: messageFollow.actorMessageFollowPayloadChecksum(parts)
  };

  const decodedLegacy = messageFollow.decodeActorMessageFollowContext(legacyContext);
  assert.deepEqual(decodedLegacy.visitedOwners, [legacyOwnerKey]);
  assert.notEqual(
    legacyOwnerKey,
    messageFollow.messageFollowOwnerFenceKey(legacyOwner)
  );

  const nextOwner = messageFollow.ownerFence({
    ownerId: 'next-owner',
    ownerLeaseGeneration: 2n,
    nodeRid: 'next-node',
    nodeRidHex: Buffer.from('next-node').toString('hex'),
    nodeGeneration: 2n,
    authorityOwnerGeneration: 2n
  });
  const advanced = messageFollow.advanceActorMessageFollowContext(
    decodedLegacy,
    legacyOwner,
    nextOwner
  );
  assert.deepEqual(advanced.visitedOwners, [
    legacyOwnerKey,
    messageFollow.messageFollowOwnerFenceKey(nextOwner)
  ]);
  assert.deepEqual(
    messageFollow.decodeActorMessageFollowContext(
      JSON.parse(JSON.stringify(advanced))
    ).visitedOwners,
    advanced.visitedOwners
  );

  const legacyOwnerWithCanonicalTextBytes = messageFollow.ownerFence({
    ...legacyOwner,
    nodeRidHex: Buffer.from(legacyOwner.nodeRid).toString('hex')
  });
  assert.throws(
    () => messageFollow.advanceActorMessageFollowContext(
      advanced,
      nextOwner,
      legacyOwnerWithCanonicalTextBytes
    ),
    /owner loop was detected/u
  );

  const opaqueOwner = messageFollow.ownerFence({
    ...legacyOwner,
    nodeRid: '\ufffd',
    nodeRidHex: 'ff'
  });
  assert.throws(
    () => messageFollow.decodeActorMessageFollowContext({
      ...legacyContext,
      sourceOwner: opaqueOwner,
      targetOwner: opaqueOwner,
      visitedOwners: [legacyKey(opaqueOwner)]
    }),
    /visited owner fence chain is invalid/u
  );

  const legacyTextCollisionOwner = messageFollow.ownerFence({
    ...legacyOwner,
    nodeRid: 'rid:ff'
  });
  assert.notEqual(
    legacyKey(legacyTextCollisionOwner),
    messageFollow.messageFollowOwnerFenceKey(opaqueOwner)
  );
  const decodedTextCollision = messageFollow.decodeActorMessageFollowContext({
    ...legacyContext,
    sourceOwner: legacyTextCollisionOwner,
    targetOwner: legacyTextCollisionOwner,
    visitedOwners: [legacyKey(legacyTextCollisionOwner)]
  });
  const exactOpaqueAdvance = messageFollow.advanceActorMessageFollowContext(
    decodedTextCollision,
    legacyTextCollisionOwner,
    opaqueOwner
  );
  assert.equal(exactOpaqueAdvance.hopCount, 1);
  assert.deepEqual(
    messageFollow.decodeActorMessageFollowContext(
      JSON.parse(JSON.stringify(exactOpaqueAdvance))
    ).visitedOwners,
    exactOpaqueAdvance.visitedOwners
  );
  parts.forEach((part) => part.close());
});

test('Message Follow rejects ambiguous legacy keys across embedded NUL boundaries', () => {
  const parts = frame('legacy-nul-boundary');
  const legacyKey = (fence) => [
    fence.nodeRid,
    fence.nodeGeneration,
    fence.ownerId,
    fence.ownerLeaseGeneration,
    fence.authorityOwnerGeneration
  ].join('\u0000');
  const nodeBoundaryFence = messageFollow.ownerFence({
    ownerId: 'owner',
    ownerLeaseGeneration: 1n,
    nodeRid: 'node\u00001',
    nodeGeneration: 2n,
    authorityOwnerGeneration: 1n
  });
  const ownerBoundaryFence = messageFollow.ownerFence({
    ownerId: '2\u0000owner',
    ownerLeaseGeneration: 1n,
    nodeRid: 'node',
    nodeGeneration: 1n,
    authorityOwnerGeneration: 1n
  });
  const ambiguousLegacyKey = legacyKey(nodeBoundaryFence);
  assert.equal(ambiguousLegacyKey, legacyKey(ownerBoundaryFence));
  assert.notEqual(
    messageFollow.messageFollowOwnerFenceKey(nodeBoundaryFence),
    messageFollow.messageFollowOwnerFenceKey(ownerBoundaryFence)
  );

  const context = (fence, visitedOwner) => ({
    operationId: 'bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb',
    objectGeneration: '1',
    sourceOwner: fence,
    targetOwner: fence,
    request: false,
    hopCount: 0,
    visitedOwners: [visitedOwner],
    payloadChecksumSha256: messageFollow.actorMessageFollowPayloadChecksum(parts)
  });
  assert.throws(
    () => messageFollow.decodeActorMessageFollowContext(
      context(nodeBoundaryFence, ambiguousLegacyKey)
    ),
    /visited owner fence chain is invalid/u
  );
  assert.throws(
    () => messageFollow.decodeActorMessageFollowContext(
      context(ownerBoundaryFence, ambiguousLegacyKey)
    ),
    /visited owner fence chain is invalid/u
  );

  const decodedCanonical = messageFollow.decodeActorMessageFollowContext(
    context(
      nodeBoundaryFence,
      messageFollow.messageFollowOwnerFenceKey(nodeBoundaryFence)
    )
  );
  const advanced = messageFollow.advanceActorMessageFollowContext(
    decodedCanonical,
    nodeBoundaryFence,
    ownerBoundaryFence
  );
  assert.equal(advanced.hopCount, 1);
  assert.deepEqual(
    messageFollow.decodeActorMessageFollowContext(
      JSON.parse(JSON.stringify(advanced))
    ).visitedOwners,
    advanced.visitedOwners
  );
  parts.forEach((part) => part.close());
});
