import assert from 'node:assert/strict';
import { createHash } from 'node:crypto';
import { readFileSync } from 'node:fs';
import { test } from 'node:test';
import {
  ServiceMaintenanceRuntime,
  classifyRelocationRecovery
} from '../../packages/framework/src/runtime/foundation/service-maintenance-runtime';
import {
  ServiceMailbox
} from '../../packages/framework/src/runtime/foundation/service-mailbox';
import {
  ZLinkInMemoryAuthorityStore
} from '../../packages/framework/src/runtime/locations/in-memory-authority-store';
import type {
  ZLinkAuthorityKey,
  ZLinkAuthoritySnapshot,
  ZLinkLocationOwnerToken,
  ZLinkObjectCreationTarget
} from '../../packages/framework/src/contracts/Locations';
import {
  decodeServiceRelocationEnvelope,
  encodeServiceRelocationEnvelope,
  type ServiceRelocationEnvelope,
  type ServiceRelocationQueuedMessage,
  type ServiceRelocationTimer
} from '../../packages/framework/src/runtime/foundation/service-relocation-runtime';
import {
  ServiceCapturedObjectRelocation,
  ServiceRelocationObjectCaptureOwner,
  ServiceRelocationObjectRestoreOwner,
  type ServiceRelocationCaptureUnit
} from '../../packages/framework/src/runtime/foundation/service-relocation-object-owner';
import {
  ZLinkManagedTimer
} from '../../packages/framework/src/runtime/spots/spot-timer';
import {
  ZLinkStatefulAuthorityRouteRuntime
} from '../../packages/framework/src/runtime/host/stateful-authority-route-runtime';
import { encodeAuthorityKey } from '../../packages/framework/src/runtime/locations/authority-key-codec';
import { encodeActorAuthorityIdentity } from '../../packages/framework/src/runtime/actors';
import {
  ZLinkFrameworkErrorKind,
  ZLinkFrameworkException,
  ZLinkSpotRelocationReadinessMode,
  ZLinkSpotRelocationReadyOutcome,
  ZLinkTimerOverrunPolicy,
  ZLinkUserSpotExecutionMode
} from '../../packages/framework/src/contracts';
import { ZLinkSpotActivation } from '../../packages/framework/src/runtime/spots/spot-activation-state';
import { ZLinkSpotSerialExecutor } from '../../packages/framework/src/runtime/spots/spot-serial-executor';
import {
  decodeMaintenanceReplyRelay,
  decodeMaintenanceReplyRelayAck,
  decodeMaintenanceRelocationControl,
  decodeSessionRelocationSeal,
  decodeServiceWireFrozenRecord,
  encodeMaintenanceRelocationControl,
  encodeMaintenanceReplyRelay,
  encodeMaintenanceReplyRelayAck,
  encodeSessionRelocationSeal,
  encodeServiceWireFrozenRecord
} from '../../packages/framework/src/runtime/foundation/service-stateful-wire-codec';
import {
  decodeRoutingId,
  encodeRoutingIdStorageHex
} from '../../packages/framework/src/runtime/routing-id';

test('stateful service wire separates opaque routing IDs from canonical UTF-8 text', () => {
  const fixture = JSON.parse(readFileSync(
    '../../runtime/protocol/golden/session-relocation-barrier-v1.json',
    'utf8'
  )) as { readonly canonical: readonly { readonly command: number; readonly hex: string }[] };
  const sealBytes = Buffer.from(
    fixture.canonical.find(entry => entry.command === 42)!.hex,
    'hex'
  );
  const seal = decodeSessionRelocationSeal(sealBytes);
  const opaqueNodeRid = decodeRoutingId('opaque-node', 'ff00fe');
  const roundTrip = decodeSessionRelocationSeal(encodeSessionRelocationSeal({
    ...seal,
    coordinator: {
      ...seal.coordinator,
      nodeRid: opaqueNodeRid as never
    }
  }));
  assert.equal(
    encodeRoutingIdStorageHex(roundTrip.coordinator.nodeRid as never),
    'ff00fe'
  );

  const ownerId = 'canonical-utf8-owner';
  const malformed = Buffer.from(encodeSessionRelocationSeal({
    ...seal,
    coordinator: { ...seal.coordinator, ownerId }
  }));
  const ownerOffset = malformed.indexOf(Buffer.from(ownerId, 'utf8'));
  assert.ok(ownerOffset > 0);
  malformed[ownerOffset] = 0xff;
  assert.throws(
    () => decodeSessionRelocationSeal(malformed),
    error => error instanceof Error
      && error.name === 'ServiceWireProtocolError'
      && /coordinatorOwnerId/.test(error.message)
  );
});

test('ApplicationSignaled relocation consumes one deferred boundary and reports exact completion', async () => {
  const completions: ZLinkSpotRelocationReadyOutcome[] = [];
  const serial = new ZLinkSpotSerialExecutor(true);
  const activation = new ZLinkSpotActivation({
    meshName: 'mesh-a',
    spotId: 'spot-a',
    domain: {
      kind: 'user',
      executionMode: ZLinkUserSpotExecutionMode.SpotWide,
      relocationReadiness: ZLinkSpotRelocationReadinessMode.ApplicationSignaled
    },
    spotType: class {} as never,
    spot: {
      async onRelocationReadyCompleted(completion: { outcome: ZLinkSpotRelocationReadyOutcome }) {
        completions.push(completion.outcome);
      }
    } as never,
    serial,
    timers: {} as never,
    actorHandlers: {} as never,
    handlers: {} as never
  });

  const boundary = activation.waitForRelocationBoundary();
  await serial.execute(() => activation.relocationReadyCall().defer());
  assert.equal(await boundary, true);
  assert.deepEqual(completions, []);
  await activation.completeConsumedRelocationBoundary(
    ZLinkSpotRelocationReadyOutcome.Continued
  );
  assert.deepEqual(completions, [ZLinkSpotRelocationReadyOutcome.Continued]);
  await activation.notifyRelocatedBoundary();
  assert.deepEqual(completions, [
    ZLinkSpotRelocationReadyOutcome.Continued,
    ZLinkSpotRelocationReadyOutcome.Relocated
  ]);
  assert.throws(
    () => activation.relocationReadyCall().defer(),
    (error: unknown) =>
      error instanceof ZLinkFrameworkException
      && error.kind === ZLinkFrameworkErrorKind.InvalidOperation
  );

  const duplicateBoundary = activation.waitForRelocationBoundary();
  await serial.execute(() => {
    const call = activation.relocationReadyCall();
    call.defer();
    assert.throws(
      () => activation.ensureContextOperationAllowed(),
      (error: unknown) =>
        error instanceof ZLinkFrameworkException
        && error.kind === ZLinkFrameworkErrorKind.InvalidOperation
    );
    assert.throws(
      () => call.defer(),
      (error: unknown) =>
        error instanceof ZLinkFrameworkException
        && error.kind === ZLinkFrameworkErrorKind.InvalidOperation
    );
  });
  assert.equal(await duplicateBoundary, true);
  await activation.completeConsumedRelocationBoundary(
    ZLinkSpotRelocationReadyOutcome.Continued
  );

  const anyTurn = new ZLinkSpotActivation({
    meshName: 'mesh-a',
    spotId: 'spot-b',
    domain: {
      kind: 'user',
      executionMode: ZLinkUserSpotExecutionMode.SpotWide,
      relocationReadiness: ZLinkSpotRelocationReadinessMode.AnyTurnBoundary
    },
    spotType: class {} as never,
    spot: {} as never,
    serial: new ZLinkSpotSerialExecutor(true),
    timers: {} as never,
    actorHandlers: {} as never,
    handlers: {} as never
  });
  assert.throws(
    () => anyTurn.relocationReadyCall().defer(),
    /requires ApplicationSignaled/
  );
});

test('ApplicationSignaled defer without active relocation completes before the next application turn', async () => {
  const events: string[] = [];
  const serial = new ZLinkSpotSerialExecutor(true);
  const activation = new ZLinkSpotActivation({
    meshName: 'mesh-a',
    spotId: 'spot-ready-without-relocation',
    domain: {
      kind: 'user',
      executionMode: ZLinkUserSpotExecutionMode.SpotWide,
      relocationReadiness: ZLinkSpotRelocationReadinessMode.ApplicationSignaled
    },
    spotType: class {} as never,
    spot: {
      async onRelocationReadyCompleted(
        completion: { outcome: ZLinkSpotRelocationReadyOutcome }
      ) {
        events.push(`completion:${completion.outcome}`);
      }
    } as never,
    serial,
    timers: {} as never,
    actorHandlers: {} as never,
    handlers: {} as never
  });

  let nextTurn!: Promise<void>;
  await serial.execute(() => {
    events.push('handler');
    const call = activation.relocationReadyCall();
    call.defer();
    assert.throws(
      () => call.defer(),
      (error: unknown) =>
        error instanceof ZLinkFrameworkException
        && error.kind === ZLinkFrameworkErrorKind.InvalidOperation
    );
    nextTurn = serial.post(() => {
      events.push('next-application-turn');
    });
  });
  await nextTurn;

  assert.deepEqual(events, [
    'handler',
    `completion:${ZLinkSpotRelocationReadyOutcome.Continued}`,
    'next-application-turn'
  ]);
});

function frozenU16(value: number): Buffer {
  const bytes = Buffer.alloc(2);
  bytes.writeUInt16BE(value);
  return bytes;
}

function frozenU32(value: number): Buffer {
  const bytes = Buffer.alloc(4);
  bytes.writeUInt32BE(value);
  return bytes;
}

function frozenU64(value: bigint): Buffer {
  const bytes = Buffer.alloc(8);
  bytes.writeBigUInt64BE(value);
  return bytes;
}

function frozenText8(value: string): Buffer {
  const bytes = Buffer.from(value);
  return Buffer.concat([Buffer.of(bytes.byteLength), bytes]);
}

function frozenText16(value: string): Buffer {
  const bytes = Buffer.from(value);
  return Buffer.concat([frozenU16(bytes.byteLength), bytes]);
}

function frozenBody16(value: Uint8Array): Buffer {
  return Buffer.concat([frozenU16(value.byteLength), value]);
}

function frozenPayload(): Buffer {
  const body = Buffer.concat([
    frozenText8('Packet'), frozenText8('application/json'), frozenU32(2), Buffer.of(1, 2)
  ]);
  return Buffer.concat([Buffer.of(1), frozenU32(body.byteLength), body]);
}

function frozenActorRef(actorId = 'actor'): Buffer {
  return Buffer.concat([frozenText8(actorId), frozenU64(3n)]);
}

function frozenSpotRef(spotId = 'spot'): Buffer {
  return Buffer.concat([frozenText8(spotId), frozenU64(4n)]);
}

function frozenActorRoute(): Buffer {
  return Buffer.concat([
    frozenActorRef(), frozenText8('node-t'), frozenU64(5n), frozenU64(6n), frozenU64(7n)
  ]);
}

function frozenSpotRoute(): Buffer {
  return Buffer.concat([
    frozenSpotRef(), frozenText8('node-t'), frozenU64(5n), frozenU64(6n), frozenU64(7n)
  ]);
}

function frozenSource(kind: number): Buffer {
  const fields = [
    frozenText8('node-s'), frozenU64(8n), frozenText8('owner-s'), frozenU64(9n)
  ];
  if (kind === 2) fields.push(frozenText8('spot-s'));
  else if (kind === 3 || kind === 4) {
    fields.push(frozenActorRef('actor-s'));
    if (kind === 4) {
      fields.push(frozenText8('session-s'), frozenU64(10n), frozenU64(11n));
    }
  }
  return Buffer.concat([Buffer.of(kind), frozenBody16(Buffer.concat(fields))]);
}

function frozenRecord(
  recordKind: number,
  sourceKind: number,
  operationKind: number,
  operationLow: bigint,
  replyRouteId: bigint | undefined,
  body: Uint8Array,
  metadata = false
): Buffer {
  const metadataFrame = metadata
    ? Buffer.concat([Buffer.of(1, 1, 1), frozenText8('trace'), frozenText16('abc')])
    : Buffer.of(0);
  const reply = replyRouteId === undefined ? Buffer.alloc(0) : frozenU64(replyRouteId);
  return Buffer.concat([
    Buffer.of(recordKind), frozenSource(sourceKind), metadataFrame,
    frozenU64(0n), frozenU64(operationLow), frozenU32(operationKind),
    frozenBody16(reply), body
  ]);
}

test('canonical frozen-record closed union validates all 14 bodies and bound-session request identity', () => {
  const payload = frozenPayload();
  const channelBody = Buffer.concat([frozenText8('channel'), payload]);
  const spotBody = Buffer.concat([frozenSpotRoute(), payload]);
  const actorBody = Buffer.concat([frozenActorRoute(), payload]);
  const snapshot = Buffer.concat([frozenActorRef(), frozenSpotRef()]);
  const instanceRoute = Buffer.concat([
    frozenText8('node-t'), frozenU64(5n), frozenText8('spot-1'), frozenText8('mesh'),
    frozenText8('instance-type'), frozenText8('descriptor-v1'), frozenU64(1000n)
  ]);
  const records = [
    frozenRecord(1, 1, 0, 0n, undefined, payload, true),
    frozenRecord(2, 4, 1, 1n, 12n, payload, true),
    frozenRecord(3, 2, 0, 0n, undefined, channelBody, true),
    frozenRecord(4, 3, 2, 2n, 13n, channelBody, true),
    frozenRecord(5, 1, 0, 3n, undefined, spotBody, true),
    frozenRecord(6, 4, 3, 4n, 14n, spotBody, true),
    frozenRecord(7, 2, 0, 0n, undefined,
      Buffer.concat([frozenText8('channel'), frozenText8('topic'), payload]), true),
    frozenRecord(8, 1, 7, 5n, undefined,
      Buffer.concat([Buffer.of(1), frozenBody16(snapshot)])),
    frozenRecord(9, 3, 0, 6n, undefined, actorBody, true),
    frozenRecord(10, 4, 4, 7n, 15n, actorBody, true),
    frozenRecord(11, 3, 4, 8n, 16n,
      Buffer.concat([frozenU32(0), frozenU32(0), Buffer.of(1), payload])),
    frozenRecord(12, 1, 0, 0n, undefined,
      Buffer.concat([Buffer.of(1), frozenBody16(frozenText8('node-t'))])),
    frozenRecord(13, 1, 0, 0n, undefined, Buffer.concat([
      Buffer.of(4, 1), frozenU64(4n), frozenU64(5n), Buffer.of(2),
      frozenBody16(Buffer.concat([frozenText8('spot-1'), frozenU64(9n), frozenU64(10n)])),
      frozenU32(0), frozenU32(0)
    ])),
    frozenRecord(14, 2, 12, 9n, 17n, Buffer.concat([
      Buffer.of(2), frozenBody16(instanceRoute), frozenU64(8n), Buffer.of(2), payload
    ]), true)
  ];
  for (const [index, record] of records.entries()) {
    const decoded = decodeServiceWireFrozenRecord(record);
    assert.equal(decoded.recordKind, index + 1);
    assert.deepEqual(encodeServiceWireFrozenRecord(decoded), record);
    assert.throws(() => decodeServiceWireFrozenRecord(record.subarray(0, -1)));
  }
  const boundRequest = decodeServiceWireFrozenRecord(records[1]!);
  assert.equal(boundRequest.sourceKind, 4);
  assert.deepEqual(boundRequest.sourceActor, { actorId: 'actor-s', generation: 3n });
  assert.equal(boundRequest.sourceSessionRid, 'session-s');
  assert.equal(boundRequest.sourceBindingGeneration, 10n);
  assert.equal(boundRequest.sourceSessionSequence, 11n);
  assert.equal(boundRequest.replyRouteId, 12n);
  assert.throws(() => encodeServiceWireFrozenRecord({ ...boundRequest, operationKind: 2 }));
  assert.throws(() =>
    decodeServiceWireFrozenRecord(frozenRecord(2, 4, 1, 0n, 12n, payload, true))
  );

  const encodedData = encodeMaintenanceRelocationControl({
    kind: 'data',
    relocation: { high: 4n, low: 5n },
    targetAttemptGeneration: 6n,
    coordinator: {
      ownerId: 'coordinator', leaseGeneration: 7n, nodeRid: 'node-a',
      nodeGeneration: 11n, expectedAuthorityStoreVersion: 'store-3'
    },
    senderRole: 'source',
    object: {
      kind: 'userSpot', spotId: 'spot-1', objectGeneration: 9n,
      expectedAuthorityOwnerGeneration: 10n
    },
    frozenRecord: boundRequest
  });
  const decodedData = decodeMaintenanceRelocationControl(encodedData);
  assert.equal(decodedData.kind, 'data');
  if (decodedData.kind === 'data' && decodedData.frozenRecord !== undefined) {
    assert.deepEqual(decodedData.frozenRecord.canonicalBytes, records[1]);
  } else {
    assert.fail('Command 31 did not preserve its canonical frozen record.');
  }
});

test('canonical command 33 and 46 match the shared reply-relay byte fixture', () => {
  const fixture = JSON.parse(readFileSync(
    '../../runtime/protocol/golden/reply-relay-v1.json',
    'utf8'
  )) as { readonly canonical: readonly { readonly name: string; readonly hex: string }[] };
  const command33 = Buffer.from(
    fixture.canonical.find(value => value.name === 'maintenanceReplyRelay')!.hex,
    'hex'
  );
  const command46 = Buffer.from(
    fixture.canonical.find(value => value.name === 'replyRelayAlreadyTerminalAck')!.hex,
    'hex'
  );
  const coordinator = {
    ownerId: 'coordinator',
    leaseGeneration: 7n,
    nodeRid: 'node-a',
    nodeGeneration: 11n,
    expectedAuthorityStoreVersion: 'store-3'
  };
  const relay = {
    relocation: { high: 4n, low: 5n },
    targetAttemptGeneration: 6n,
    coordinator,
    operation: { high: 1n, low: 2n },
    replyRouteId: 3n,
    participantId: 8n,
    sequence: 9n,
    terminalResult: 101,
    failureCode: 0
  };
  assert.deepEqual(encodeMaintenanceReplyRelay(relay), command33);
  assert.deepEqual(decodeMaintenanceReplyRelay(command33), relay);
  assert.throws(() => encodeMaintenanceReplyRelay({
    ...relay,
    terminalResult: 105,
    failureCode: 1
  }), /does not match/);
  assert.throws(() => encodeMaintenanceReplyRelay({
    ...relay,
    payload: {
      packetName: 'Reply',
      contentType: 'application/json',
      bytes: Buffer.from('{}')
    }
  }), /must not carry a payload/);

  const ack = {
    relocation: { high: 4n, low: 5n },
    coordinator,
    operation: { high: 1n, low: 2n },
    replyRouteId: 3n,
    requestSource: {
      ownerId: 'source',
      leaseGeneration: 13n,
      nodeRid: 'node-s',
      nodeGeneration: 17n
    },
    status: 'alreadyTerminal' as const
  };
  assert.deepEqual(encodeMaintenanceReplyRelayAck(ack), command46);
  assert.deepEqual(decodeMaintenanceReplyRelayAck(command46), ack);
  assert.throws(() => encodeMaintenanceReplyRelayAck({
    ...ack,
    replyRouteId: 0n
  }), /non-zero u64/);
  assert.throws(() => decodeMaintenanceReplyRelay(command33.subarray(0, -1)));
  assert.throws(() => decodeMaintenanceReplyRelayAck(Buffer.concat([command46, Buffer.of(0)])));
});

test('startup authority scan submits published Actor roots before admission', async () => {
  const authority = new ZLinkInMemoryAuthorityStore({
    isTargetLive: () => true
  });
  const actor = await createActorAuthority(authority, 'actor-restart');
  const recovered: ZLinkAuthoritySnapshot[] = [];
  const runtime = new ZLinkStatefulAuthorityRouteRuntime({
    store: authority,
    meshNodes: new Map(),
    pollingIntervalMs: 60_000,
    pageSize: 32,
    reportError: error => { throw error; },
    recoverActor: async snapshot => {
      recovered.push(snapshot);
    }
  });

  await runtime.start();
  await runtime.stop();

  assert.equal(recovered.length, 1);
  assert.equal(recovered[0]?.storeVersion.value, actor.storeVersion.value);
  assert.equal(recovered[0]?.allocation.objectKind, 'actor');
});

test('Retire preflight precedes publication and starts every ready unit without relocation permits', async () => {
  const events: string[] = [];
  const states: string[] = [];
  let active = 0;
  let peak = 0;
  const runtime = new ServiceMaintenanceRuntime({
    preflight: async () => {
      events.push('preflight');
      return true;
    },
    publishState: state => events.push(state),
    forceStop: () => {
      events.push('force');
    }
  });
  runtime.observe(snapshot => states.push(snapshot.state));
  for (let index = 0; index < 4; index++) {
    runtime.enqueue({
      id: `unit-${index}`,
      ready: () => true,
      relocate: async () => {
        active++;
        peak = Math.max(peak, active);
        await new Promise(resolve => setImmediate(resolve));
        active--;
      }
    });
  }
  const terminal = await runtime.start('retire', 2_000);
  assert.equal(terminal.state, 'completed');
  assert.equal(peak, 4);
  assert.deepEqual(events.slice(0, 3), ['preflight', 'retiring', 'draining']);
  assert.deepEqual(states.slice(0, 4), [
    'serving',
    'serving',
    'serving',
    'serving'
  ]);
  assert.ok(states.includes('preparing'));
  assert.ok(states.includes('retiring'));
  assert.ok(states.includes('draining'));
  assert.equal(states.at(-1), 'completed');
});

test('external maintenance abort cancels an admitted relocation unit', async () => {
  const abort = new AbortController();
  let started!: () => void;
  let observedAbort = false;
  let forced = 0;
  const startedPromise = new Promise<void>(resolve => { started = resolve; });
  const runtime = new ServiceMaintenanceRuntime({
    preflight: async () => true,
    publishState: () => undefined,
    forceStop: () => { forced++; }
  });
  runtime.enqueue({
    id: 'active',
    ready: () => true,
    relocate: signal => new Promise<void>((_resolve, reject) => {
      started();
      signal.addEventListener('abort', () => {
        observedAbort = true;
        reject(signal.reason);
      }, { once: true });
    })
  });

  const operation = runtime.start('retire', 2_000, undefined, abort.signal);
  await startedPromise;
  abort.abort(new Error('Relocation deadline exceeded.'));
  const terminal = await operation;
  assert.equal(terminal.state, 'forceStopped');
  assert.equal(observedAbort, true);
  assert.equal(forced, 1);
});

test('first maintenance intent wins and blocked preflight keeps admission uncommitted', async () => {
  const runtime = new ServiceMaintenanceRuntime({
    preflight: async () => false,
    publishState: () => assert.fail('blocked preflight must not publish host state'),
    forceStop: () => assert.fail('preflight failure must not force stop')
  });
  const first = runtime.start('retire', 100);
  const second = runtime.start('shutdown', 100);
  assert.equal(first, second);
  const terminal = await first;
  assert.equal(terminal.kind, 'retire');
  assert.equal(terminal.state, 'blocked');
});

test('deadline after publication forces bounded terminal shutdown and observers see it', async () => {
  const states: string[] = [];
  let forced = 0;
  const runtime = new ServiceMaintenanceRuntime({
    preflight: async () => true,
    publishState: () => {},
    forceStop: () => {
      forced++;
    }
  });
  runtime.observe(snapshot => states.push(snapshot.state));
  runtime.enqueue({
    id: 'slow',
    ready: () => true,
    relocate: signal => new Promise((_, reject) => {
      signal.addEventListener('abort', () => reject(signal.reason), { once: true });
    })
  });
  const terminal = await runtime.start('retire', 5);
  assert.equal(terminal.state, 'forceStopped');
  assert.equal(forced, 1);
  assert.equal(states.includes('forceStopped'), true);
});

test('recovery never rolls a published missing or corrupt root back to source', () => {
  assert.equal(classifyRelocationRecovery(false, true, true, true), 'orphan');
  assert.equal(classifyRelocationRecovery(true, false, true, true), 'relocationDataLost');
  assert.equal(classifyRelocationRecovery(true, true, false, true), 'relocationDataLost');
  assert.equal(classifyRelocationRecovery(true, true, true, false), 'relocationDataLost');
  assert.equal(classifyRelocationRecovery(true, true, true, true), 'resume');
});

test('relocation envelope preserves queued work and logical timers deterministically', () => {
  const envelope = relocationEnvelope();
  const encoded = encodeServiceRelocationEnvelope(envelope);
  const decoded = decodeServiceRelocationEnvelope(encoded);
  assert.deepEqual(
    decoded.participants.map(({ participantId }) => participantId),
    [1n, 2n]
  );
  assert.deepEqual(
    decoded.participants[0]?.rootSpotId,
    'room'
  );
  const actor = decoded.participants[0]!;
  const spot = decoded.participants[1]!;
  assert.deepEqual(
    actor.queuedMessages.map(({ sequence }) => sequence),
    [1n]
  );
  assert.deepEqual(
    spot.timers.map(({ timerId }) => timerId),
    ['heartbeat', 'idle']
  );
  assert.deepEqual(
    decoded.participants.map(participant => Buffer.from(participant.applicationState).toString('utf8')),
    ['actor-state', 'spot-state']
  );

  const standalone = envelope.participants.find(value => value.objectKind === 'actor')!;
  const standaloneEncoded = encodeServiceRelocationEnvelope({
    aggregateId: '22222222-2222-4222-8222-222222222222',
    aggregateGeneration: 1n,
    participants: [standalone],
    memberships: []
  });
  assert.notEqual(standaloneEncoded[0], 0x7b, 'standalone Actor must not use the legacy JSON envelope');
  const standaloneDecoded = decodeServiceRelocationEnvelope(standaloneEncoded);
  assert.deepEqual(standaloneDecoded.participants[0]?.participantId, 1n);
  assert.deepEqual(standaloneDecoded.participants[0]?.rootObjectKind, 'actor');
  assert.deepEqual(standaloneDecoded.participants[0]?.rootSpotId, 'a');
  assert.deepEqual(standaloneDecoded.participants[0]?.queuedMessages, standalone.queuedMessages);
});

test('relocation envelope rejects malformed root and trailing stream bytes', () => {
  const encoded = encodeServiceRelocationEnvelope(relocationEnvelope());
  const malformedRoot = Buffer.from(encoded);
  malformedRoot[16] = 4;
  assert.throws(
    () => decodeServiceRelocationEnvelope(malformedRoot),
    /root object kind is invalid/
  );
  assert.throws(
    () => decodeServiceRelocationEnvelope(Buffer.concat([encoded, Buffer.of(0)])),
    /Trailing relocation envelope bytes/
  );
});

test('relocation envelope rejects duplicate participant queue and timer identities', () => {
  const envelope = relocationEnvelope();
  const actor = envelope.participants.find(value => value.objectKind === 'actor')!;
  const spot = envelope.participants.find(value => value.objectKind === 'user_spot')!;
  const queueEnvelope: ServiceRelocationEnvelope = {
    ...envelope,
    participants: envelope.participants.map(value => value === actor ? {
      ...actor,
      queuedMessages: [actor.queuedMessages[0]!, actor.queuedMessages[0]!]
    } : value)
  };
  assert.throws(
    () => encodeServiceRelocationEnvelope(queueEnvelope),
    /queue sequences must be unique per participant/
  );

  const timerEnvelope: ServiceRelocationEnvelope = {
    ...envelope,
    participants: envelope.participants.map(value => value === spot ? {
      ...spot,
      timers: [spot.timers[0]!, spot.timers[0]!]
    } : value)
  };
  assert.throws(
    () => encodeServiceRelocationEnvelope(timerEnvelope),
    /timer ids must be unique per participant/
  );
});

test('relocation inventory preserves 10,100 participants without a Spot member count ceiling', () => {
  const base = relocationEnvelope();
  const spot = {
    ...base.participants[0]!,
    queuedMessages: [],
    timers: []
  };
  const actors = Array.from({ length: 10_099 }, (_, index) => ({
    key: `actor:profile:${index.toString().padStart(5, '0')}`,
    objectKind: 'actor' as const,
    stableType: 'ProfileActor',
    objectGeneration: 1n,
    authorityOwnerGeneration: 1n,
    applicationState: Buffer.alloc(0),
    boundSessionState: Buffer.alloc(0),
    queuedMessages: [],
    timers: []
  }));
  const envelope: ServiceRelocationEnvelope = {
    aggregateId: '66666666-6666-4666-8666-666666666666',
    aggregateGeneration: 1n,
    participants: [spot, ...actors],
    memberships: actors.map((actor, index) => ({
      actorKey: actor.key,
      spotKey: spot.key,
      spotObjectGeneration: spot.objectGeneration,
      membershipEpoch: BigInt(index + 1)
    }))
  };

  const decoded = decodeServiceRelocationEnvelope(
    encodeServiceRelocationEnvelope(envelope)
  );
  assert.equal(decoded.participants.length, 10_100);
  assert.equal(decoded.memberships.length, 0);
  assert.equal(decoded.participants[0]?.participantId, 1n);
  assert.equal(decoded.participants.at(-1)?.participantId, 10_100n);
});

test('mailbox seal captures queued work, holds new ingress, and restores or relays in order', () => {
  const mailbox = new ServiceMailbox({
    applicationMessages: 16,
    applicationBytes: 1_024,
    infrastructureMessages: 4,
    infrastructureBytes: 256
  });
  assert.equal(mailbox.tryEnqueue(mailboxRecord('spot:room', 'application', 'one')), true);
  assert.equal(mailbox.tryEnqueue(mailboxRecord('spot:room', 'application', 'two')), true);
  assert.equal(mailbox.tryEnqueue(mailboxRecord('node', 'infrastructure', 'probe')), true);
  const first = mailbox.trySealApplicationOwner('spot:room');
  assert.ok(first);
  assert.deepEqual(first.captured.map(firstPart), ['one', 'two']);
  assert.equal(mailbox.tryEnqueue(mailboxRecord('spot:room', 'application', 'three')), true);
  assert.equal(mailbox.tryClaim('application', 16, 1_024), undefined);
  const infrastructure = mailbox.tryClaim('infrastructure', 4, 256);
  assert.ok(infrastructure);
  assert.equal(firstPart(infrastructure.records[0]!), 'probe');
  assert.equal(mailbox.release(infrastructure), true);

  assert.equal(mailbox.abortRelocation(first), true);
  const restored = mailbox.tryClaim('application', 16, 1_024);
  assert.ok(restored);
  assert.deepEqual(restored.records.map(firstPart), ['one', 'two', 'three']);
  assert.equal(mailbox.release(restored), true);

  assert.equal(mailbox.tryEnqueue(mailboxRecord('spot:room', 'application', 'four')), true);
  const second = mailbox.trySealApplicationOwner('spot:room');
  assert.ok(second);
  assert.equal(mailbox.tryEnqueue(mailboxRecord('spot:room', 'application', 'five')), true);
  const relay = mailbox.commitRelocation(second);
  assert.deepEqual(relay?.map(firstPart), ['five']);
  assert.equal(mailbox.pendingMessages('application'), 0);
  assert.equal(mailbox.tryEnqueue(mailboxRecord('spot:room', 'application', 'six')), false);
  mailbox.close();
});

test('relocation ingress hold adds no relocation-specific message or byte cap', () => {
  const byCount = new ServiceMailbox({
    applicationMessages: 2048,
    applicationBytes: 32 * 1024 * 1024,
    infrastructureMessages: 4,
    infrastructureBytes: 256
  });
  const countSeal = byCount.trySealApplicationOwner('spot:count');
  assert.ok(countSeal);
  for (let index = 0; index < 1024; index++) {
    assert.equal(
      byCount.tryEnqueue(mailboxRecord('spot:count', 'application', 'x')),
      true
    );
  }
  assert.equal(
    byCount.tryEnqueue(mailboxRecord('spot:count', 'application', 'overflow')),
    true
  );
  assert.equal(byCount.abortRelocation(countSeal), true);
  const restored = byCount.tryClaim('application', 2048, 32 * 1024 * 1024);
  assert.equal(restored?.records.length, 1025);
  assert.ok(restored);
  assert.equal(byCount.release(restored), true);
  byCount.close();

  const byBytes = new ServiceMailbox({
    applicationMessages: 8,
    applicationBytes: 32 * 1024 * 1024,
    infrastructureMessages: 4,
    infrastructureBytes: 256
  });
  const byteSeal = byBytes.trySealApplicationOwner('spot:bytes');
  assert.ok(byteSeal);
  assert.equal(byBytes.tryEnqueue({
    owner: 'spot:bytes',
    domain: 'application',
    parts: [Buffer.alloc(16 * 1024 * 1024)]
  }), true);
  assert.equal(byBytes.tryEnqueue({
    owner: 'spot:bytes',
    domain: 'application',
    parts: [Buffer.alloc(1)]
  }), true);
  assert.equal(byBytes.commitRelocation(byteSeal)?.length, 2);
  byBytes.close();
});

test('managed timer pauses and restores its logical schedule without native handles', async () => {
  const options = {
    overrunPolicy: ZLinkTimerOverrunPolicy.CatchUpBounded,
    maxCatchUpTicks: 3,
    stopOnUnhandledException: true
  };
  const source = new ZLinkManagedTimer('heartbeat', 60_000, options, async () => {});
  const captured = await source.captureRelocation();
  assert.equal(captured.name, 'heartbeat');
  assert.equal(captured.periodMs, 60_000);
  assert.equal(captured.handlerType, '');
  assert.deepEqual(captured.pendingTicks, []);

  const target = new ZLinkManagedTimer('heartbeat', 60_000, options, async () => {});
  target.restoreRelocation(captured);
  const restored = await target.captureRelocation();
  assert.deepEqual(restored, captured);
  await source.dispose();
  await target.dispose();
});

test('source rollback attempts every unit when one cleanup fence is stale', async () => {
  const events: string[] = [];
  const units = [
    {
      async abortSeal() {
        events.push('spot-abort');
        throw new Error('stale Message Follow abort fence');
      }
    },
    {
      async abortSeal() {
        events.push('actor-rollback');
      }
    }
  ] as unknown as ServiceRelocationCaptureUnit[];
  const captured = new ServiceCapturedObjectRelocation(relocationEnvelope(), units);
  await assert.rejects(captured.abortSource(), AggregateError);
  assert.deepEqual(events, ['actor-rollback', 'spot-abort']);
  await captured.abortSource();
  assert.deepEqual(events, ['actor-rollback', 'spot-abort']);
});

test('SpotWide Capture and Restore callbacks run eight at a time and preserve participant order', async () => {
  let captureActive = 0;
  let capturePeak = 0;
  const captureReleases: Array<() => void> = [];
  const makeUnit = (
    index: number,
    objectKind: 'user_spot' | 'actor'
  ): ServiceRelocationCaptureUnit => ({
    authorityKey: objectKind === 'user_spot' ? 'spot:bounded' : `actor:${index}`,
    objectKind,
    stableType: objectKind === 'user_spot' ? 'room' : 'player',
    objectGeneration: 1n,
    authorityOwnerGeneration: 1n,
    async seal() {
      return { boundSessionState: Buffer.alloc(0), queuedMessages: [], timers: [] };
    },
    async captureApplicationState() {
      captureActive++;
      capturePeak = Math.max(capturePeak, captureActive);
      await new Promise<void>(resolve => captureReleases.push(resolve));
      captureActive--;
      return Buffer.from(String(index));
    },
    commitSeal() {},
    abortSeal() {}
  });
  const spot = makeUnit(0, 'user_spot');
  const actors = Array.from({ length: 16 }, (_, index) => makeUnit(index + 1, 'actor'));
  const memberships = actors.map((actor, index) => ({
    actorKey: actor.authorityKey,
    spotKey: spot.authorityKey,
    spotObjectGeneration: 1n,
    membershipEpoch: BigInt(index + 1)
  }));
  const capturing = new ServiceRelocationObjectCaptureOwner(8).captureUserSpotAggregate(
    '55555555-5555-4555-8555-555555555555',
    1n,
    spot,
    actors,
    memberships
  );
  await waitUntil(() => captureReleases.length === 8);
  assert.equal(capturePeak, 8);
  while (captureReleases.length > 0) {
    captureReleases.splice(0).forEach(resolve => resolve());
    await new Promise(resolve => setImmediate(resolve));
  }
  const captured = await capturing;
  assert.deepEqual(
    captured.envelope.participants.map(value => Buffer.from(value.applicationState).toString()),
    Array.from({ length: 17 }, (_, index) => String(index))
  );

  let restoreActive = 0;
  let restorePeak = 0;
  const restoreReleases: Array<() => void> = [];
  const restored: string[] = [];
  const restoreOwner = new ServiceRelocationObjectRestoreOwner({
    async createHidden(participant) {
      return { authorityKey: participant.key };
    },
    async restoreApplicationState(_hidden, payload) {
      restoreActive++;
      restorePeak = Math.max(restorePeak, restoreActive);
      await new Promise<void>(resolve => restoreReleases.push(resolve));
      restored.push(Buffer.from(payload).toString());
      restoreActive--;
    },
    async restoreMemberships() {},
    async publish() {},
    async restoreBoundSession() {},
    async replayQueuedMessage() {},
    async restoreTimer() {},
    async normalize() {},
    async openAdmission() {},
    abort() {}
  }, authorityKey, 8);
  const restoring = restoreOwner.prepare(captured.envelope);
  await waitUntil(() => restoreReleases.length === 8);
  assert.equal(restorePeak, 8);
  while (restoreReleases.length > 0) {
    restoreReleases.splice(0).forEach(resolve => resolve());
    await new Promise(resolve => setImmediate(resolve));
  }
  await restoring;
  assert.equal(restored.length, 17);
});

test('concrete standalone Actor owner preserves external membership and exact generation', async () => {
  const events: string[] = [];
  const captureOwner = new ServiceRelocationObjectCaptureOwner();
  const actor = captureUnit(events, {
    authorityKey: 'actor:solo',
    objectKind: 'actor',
    stableType: 'player',
    objectGeneration: 13n,
    authorityOwnerGeneration: 7n,
    state: 'solo-state',
    journal: 'solo-journal',
    queuedMessages: [{ sequence: 9n, payload: Buffer.from('solo-job') }],
    timers: []
  });
  const captured = await captureOwner.captureStandaloneActor(
    '33333333-3333-4333-8333-333333333333',
    4n,
    actor,
    {
      actorKey: 'actor:solo',
      spotKey: 'entry:node-a',
      spotObjectGeneration: 6n,
      membershipEpoch: 11n
    }
  );
  const restored: string[] = [];
  const ownerRuntime = new ServiceRelocationObjectRestoreOwner({
    async createHidden(participant) {
      restored.push(`factory:${participant.objectGeneration}:${participant.authorityOwnerGeneration}`);
      return { authorityKey: participant.key };
    },
    async restoreApplicationState(_hidden, payload) {
      restored.push(`state:${Buffer.from(payload).toString()}`);
    },
    async restoreMemberships(_hidden, memberships) {
      restored.push(`membership:${memberships[0]?.spotKey}:${memberships[0]?.membershipEpoch}`);
    },
    async publish() {},
    async restoreBoundSession() {},
    async replayQueuedMessage() {},
    async restoreTimer() {},
    async normalize() {},
    async openAdmission() {},
    abort() {
      restored.push('abort');
    }
  }, authorityKey);
  const staging = await ownerRuntime.prepare(captured.envelope);
  assert.deepEqual(restored, [
    'factory:13:7',
    'state:solo-state',
    'membership:entry:node-a:11'
  ]);
  assert.equal(staging.envelope.participants[0]?.queuedMessages[0]?.sequence, 9n);
  await ownerRuntime.abort(staging);
  assert.equal(restored.at(-1), 'abort');
  await captured.abortSource();
  assert.equal(events.at(-1), 'source-abort:actor:solo');
});

test('concrete Instance Spot aggregate preserves Actor membership and relocation payload', async () => {
  const events: string[] = [];
  const captureOwner = new ServiceRelocationObjectCaptureOwner();
  const spot = captureUnit(events, {
    authorityKey: 'instance:match:42',
    objectKind: 'instance_spot',
    stableType: 'match',
    objectGeneration: 5n,
    authorityOwnerGeneration: 8n,
    state: 'instance-state',
    journal: 'instance-journal',
    queuedMessages: [],
    timers: []
  });
  const actor = captureUnit(events, {
    authorityKey: 'actor:player',
    objectKind: 'actor',
    stableType: 'player',
    objectGeneration: 9n,
    authorityOwnerGeneration: 4n,
    state: 'actor-state',
    journal: 'actor-journal',
    queuedMessages: [],
    timers: []
  });

  const captured = await captureOwner.captureInstanceSpotAggregate(
    '44444444-4444-4444-8444-444444444444',
    3n,
    spot,
    [actor],
    [{
      actorKey: 'actor:player',
      spotKey: 'instance:match:42',
      spotObjectGeneration: 5n,
      membershipEpoch: 12n
    }]
  );

  assert.deepEqual(
    captured.envelope.participants.map(participant => participant.objectKind),
    ['instance_spot', 'actor']
  );
  assert.equal(captured.envelope.memberships[0]?.membershipEpoch, 12n);
  await captured.abortSource();
  assert.deepEqual(events.slice(-2), [
    'source-abort:actor:player',
    'source-abort:instance:match:42'
  ]);
});

function captureUnit(
  events: string[],
  options: {
    readonly authorityKey: string;
    readonly objectKind: 'actor' | 'user_spot' | 'instance_spot';
    readonly stableType: string;
    readonly objectGeneration: bigint;
    readonly authorityOwnerGeneration: bigint;
    readonly state: string;
    readonly journal: string;
    readonly queuedMessages: readonly ServiceRelocationQueuedMessage[];
    readonly timers: readonly ServiceRelocationTimer[];
  }
): ServiceRelocationCaptureUnit {
  return {
    authorityKey: options.authorityKey,
    objectKind: options.objectKind,
    stableType: options.stableType,
    objectGeneration: options.objectGeneration,
    authorityOwnerGeneration: options.authorityOwnerGeneration,
    async seal() {
      events.push(`source-seal:${options.authorityKey}`);
      return {
        boundSessionState: Buffer.from(options.journal),
        queuedMessages: options.queuedMessages,
        timers: options.timers
      };
    },
    async captureApplicationState() {
      events.push(`source-capture:${options.authorityKey}`);
      return Buffer.from(options.state);
    },
    commitSeal() {
      events.push(`source-commit:${options.authorityKey}`);
    },
    abortSeal() {
      events.push(`source-abort:${options.authorityKey}`);
    }
  };
}

async function waitUntil(predicate: () => boolean): Promise<void> {
  for (let attempt = 0; attempt < 1000; attempt++) {
    if (predicate()) return;
    await new Promise(resolve => setImmediate(resolve));
  }
  throw new Error('Timed out waiting for the relocation callback gate.');
}

async function createActorAuthority(
  authority: ZLinkInMemoryAuthorityStore,
  actorId: string
): Promise<ZLinkAuthoritySnapshot> {
  const target: ZLinkObjectCreationTarget = {
    meshName: 'mesh',
    nodeRid: 'node-a',
    nodeLifecycleGeneration: 1n,
    owner: owner('owner-a', 1n)
  };
  const payload = encodeActorAuthorityIdentity({
    actorType: 'player',
    actor: {
      actorId,
      objectGeneration: 1n,
      meshName: 'mesh',
      nodeRid: 'node-a'
    },
    meshName: 'mesh',
    ownerNodeGeneration: 1n,
    owner: target.owner,
    spotId: 'room-a',
    spotGeneration: 1n
  });
  const reserved = await authority.reserve({
    key: { kind: 'actor', globalId: actorId },
    intent: {
      stableType: 'player',
      requestContentReference: `request:${actorId}`,
      requestSha256: Buffer.alloc(32, 2),
      requestEncodedSize: BigInt(payload.byteLength)
    },
    target,
    creatingPayload: payload,
    capacity: { actors: 1, spots: 0 }
  });
  assert.equal(reserved.kind, 'reserved');
  if (reserved.kind !== 'reserved') throw new Error('Actor authority reservation failed.');
  const terminalEnvelope = Buffer.from(`created:${actorId}`, 'utf8');
  const committed = await authority.completeCreation({
    key: { kind: 'actor', globalId: actorId },
    reservationId: reserved.reservationId,
    expectedStoreVersion: reserved.creating.storeVersion.value,
    target,
    completion: {
      kind: 'created',
      readyPayload: payload,
      terminal: {
        operation: {
          sourceNodeRid: 'node-a',
          sourceNodeGeneration: 1n,
          operationId: { high: 1n, low: 2n }
        },
        terminalEnvelope,
        terminalEnvelopeSha256: createHash('sha256').update(terminalEnvelope).digest(),
        operationDeadline: new Date(Date.now() + 60_000)
      }
    }
  });
  assert.equal(committed.kind, 'created');
  if (committed.kind !== 'created') throw new Error('Actor authority completion failed.');
  return committed.ready;
}

function authorityKey(value: string): ZLinkAuthorityKey {
  return encodeAuthorityKey('user_spot', value);
}

function owner(ownerId: string, leaseGeneration: bigint): ZLinkLocationOwnerToken {
  return { ownerId, leaseGeneration };
}

function relocationEnvelope(): ServiceRelocationEnvelope {
  return {
    aggregateId: '11111111-1111-4111-8111-111111111111',
    aggregateGeneration: 1n,
    memberships: [{
      actorKey: 'actor:a',
      spotKey: 'spot:room',
      spotObjectGeneration: 3n,
      membershipEpoch: 4n
    }],
    participants: [
      {
        key: 'spot:room',
        objectKind: 'user_spot',
        stableType: 'room',
        objectGeneration: 3n,
        authorityOwnerGeneration: 2n,
        applicationState: Buffer.from('spot-state'),
        boundSessionState: Buffer.from('spot-journal'),
        queuedMessages: [],
        timers: [
          {
            timerId: 'idle',
            handlerType: 'IdleTimerHandler',
            startedAtUnixMs: 100,
            dueAtUnixMs: 1_000,
            intervalMs: 900,
            deliveryIndex: 0n,
            lastScheduledIndex: 0n,
            overrunPolicy: 'skipLateTicks',
            maxCatchUpTicks: 1,
            stopOnUnhandledException: false,
            pendingTicks: []
          },
          {
            timerId: 'heartbeat',
            handlerType: 'HeartbeatTimerHandler',
            startedAtUnixMs: 100,
            dueAtUnixMs: 500,
            intervalMs: 100,
            deliveryIndex: 3n,
            lastScheduledIndex: 4n,
            overrunPolicy: 'catchUpBounded',
            maxCatchUpTicks: 2,
            stopOnUnhandledException: true,
            pendingTicks: [{
              deliveryIndex: 4n,
              scheduledIndex: 5n,
              scheduledAtUnixMs: 600,
              skippedTicks: 0n
            }]
          }
        ]
      },
      {
        key: 'actor:a',
        objectKind: 'actor',
        stableType: 'player',
        objectGeneration: 8n,
        authorityOwnerGeneration: 5n,
        applicationState: Buffer.from('actor-state'),
        boundSessionState: Buffer.from('actor-journal'),
        queuedMessages: [{
          sequence: 1n,
          payload: frozenRecord(9, 1, 0, 1n, undefined,
            Buffer.concat([frozenActorRoute(), frozenPayload()]), true)
        }],
        timers: []
      }
    ]
  };
}

function mailboxRecord(
  owner: string,
  domain: 'application' | 'infrastructure',
  value: string
) {
  return { owner, domain, parts: [Buffer.from(value)] } as const;
}

function firstPart(record: { readonly parts: readonly Uint8Array[] }): string {
  return Buffer.from(record.parts[0]!).toString();
}
