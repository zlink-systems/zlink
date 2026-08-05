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
    authorityOwnerGeneration: 2n
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
  let requestSource = {
    ownerId: 'source-owner',
    ownerLeaseGeneration: 1n,
    nodeRid: 'source-node',
    nodeGeneration: 1n
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
    requestSource: () => requestSource,
    onMarker: (marker, actorId, index) => markers.push({ marker, actorId, index })
  });
  return {
    coordinator,
    followed,
    messageFollowPayloads,
    markers,
    setCurrentGeneration(value) { currentGeneration = value; },
    setCurrentNodeRid(value) { currentNodeRid = value; }
    ,
    setCurrentOwner(value) { currentOwner = value; },
    setRequestSource(value) { requestSource = value; }
  };
}

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
    await assert.rejects(
      coordinator.capture('actor-1', parts, false, undefined, actorRef(1n)),
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
      ownerId: 'source-owner',
      ownerLeaseGeneration: 1n,
      nodeRid: 'source-node',
      nodeGeneration: 1n
    })
  });
  coordinator.begin('actor-1', 1n);
  coordinator.snapshot('actor-1');
  coordinator.complete('actor-1', target(), targetActorRef());

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
      ownerId: 'source-owner',
      ownerLeaseGeneration: 1n,
      nodeRid: 'source-node',
      nodeGeneration: 1n
    })
  });
  coordinator.begin('actor-1', 1n);
  coordinator.snapshot('actor-1');
  coordinator.complete('actor-1', target(), targetActorRef());

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

test('Message Follow route rejects a queue above 1024 messages and emits its rejection marker', async () => {
  const markers = [];
  const coordinator = new framework.ZLinkActorHandoffCoordinator({
    routedTransport: { sendToSpot: async () => new Promise(() => {}) },
    messageFollowDurationMs: 60_000,
    requestSource: () => ({
      ownerId: 'source-owner',
      ownerLeaseGeneration: 1n,
      nodeRid: 'source-node',
      nodeGeneration: 1n
    }),
    onMarker: (marker, actorId, index) => markers.push({ marker, actorId, index })
  });
  coordinator.begin('actor-1', 1n);
  coordinator.snapshot('actor-1');
  coordinator.complete('actor-1', target(), targetActorRef());
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
  const overflow = frame('overflow');
  assert.throws(
    () => coordinator.capture(
      'actor-1',
      overflow,
      false,
      undefined,
      contextRef(overflow, { operationId: 'ffffffffffffffffffffffffffffffff' })
    ),
    (error) => error.kind === framework.ZLinkFrameworkErrorKind.Unavailable
  );
  overflow.forEach((part) => part.close());
  assert.equal(
    markers.some((entry) => entry.marker === 'message_follow_rejected'),
    true
  );
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
    backlog.map((packet) => ({ index: packet.index, ok: true }))
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
    backlog.map((packet) => ({ index: packet.index, ok: true }))
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

test('Message Follow relays before duration expiry and rejects after route removal', async () => {
  const { coordinator, followed, markers } = harness(10);
  coordinator.begin('actor-1', 1n);
  coordinator.snapshot('actor-1');
  coordinator.complete('actor-1', target(), targetActorRef());

  const inside = frame('G1');
  await coordinator.capture('actor-1', inside, false, undefined, contextRef(inside));
  inside.forEach((part) => part.close());
  assert.deepEqual(followed, ['G1']);

  await new Promise((resolve) => setTimeout(resolve, 20));
  const outside = frame('G2');
  await assert.rejects(
    coordinator.capture('actor-1', outside, false, undefined, contextRef(outside)),
    (error) => error.kind === framework.ZLinkFrameworkErrorKind.Unavailable
  );
  outside.forEach((part) => part.close());
  assert.equal(markers.some((entry) => entry.marker === 'message_follow_route_removed'), true);
  assert.equal(markers.some((entry) => entry.marker === 'message_follow_expired'), true);
});

test('a Core-routed packet owned by the current actor bypasses an older Message Follow route', async () => {
  const { coordinator, followed, markers } = harness();
  coordinator.begin('actor-1', 1n);
  coordinator.snapshot('actor-1');
  coordinator.complete('actor-1', target(), targetActorRef());

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
  coordinator.complete('actor-1', target(), targetActorRef('target', 1n));
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
    ownerId: firstFence.ownerId,
    ownerLeaseGeneration: BigInt(firstFence.ownerLeaseGeneration),
    nodeRid: firstFence.nodeRid,
    nodeGeneration: BigInt(firstFence.nodeGeneration)
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
      ownerId: 'source-owner',
      ownerLeaseGeneration: 1n,
      nodeRid: 'source-node',
      nodeGeneration: 1n
    })
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
  const { coordinator } = harness();
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
  coordinator.complete('actor-1', target(), targetActorRef(), [terminal]);
  assert.equal(
    coordinator.acceptRelocatedTerminal('actor-1', backlog[0], terminal, 'target-node', 2n),
    'terminalReceived'
  );
  assert.deepEqual(await pendingReply, { marker: 'R1', requestSeq: '731' });

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
  coordinator.complete('actor-1', target('late'), targetActorRef('late', 3n), [lateTerminal]);
  assert.equal(
    coordinator.acceptRelocatedTerminal(
      'actor-1', lateBacklog[0], lateTerminal, 'late-node', 2n
    ),
    'terminalReceived'
  );
  assert.deepEqual(await lateReply, { marker: 'late' });
});
