const assert = require('node:assert/strict');
const test = require('node:test');
const { completionFixture, example, entry } = require('./helpers/relocation-completion');
const completionStore = require('../../packages/framework/dist/runtime/locations/relocation-completion-store');
const wire = require('../../packages/framework/dist/runtime/foundation/service-stateful-wire-codec');
const multipart = require('../../packages/framework/dist/runtime/foundation/service-wire-m6a-codec');
const {
  ZLinkFrameworkErrorKind
} = require('../../packages/framework/dist/contracts/Errors/ZLinkFrameworkException');
const { RequestResult } = require('../../packages/framework/dist/runtime/backend/runtime-values');
const {
  ServiceWireFrameworkErrorCode
} = require('../../packages/framework/dist/runtime/foundation/service-wire-constants.generated');
const {
  encodeRelocationCompletionRecord
} = require('../../packages/framework/dist/runtime/locations/relocation-completion-record');
const { ZLinkHostServiceRelocationRuntime } = require('../../packages/framework/dist/internal');

test('blob bound replaces only the new result with requestFailed and preserves earlier entries', () => {
  const original = {
    relocation: { high: 0n, low: 1n },
    targetAttemptGeneration: 1n,
    coordinator: {
      ownerId: 'target',
      leaseGeneration: 1n,
      nodeRid: 'target',
      nodeGeneration: 1n,
      expectedAuthorityStoreVersion: '1'
    },
    operation: { high: 1n, low: 2n },
    replyRouteId: 3n,
    participantId: 1n,
    sequence: 1n,
    terminalResult: RequestResult.Ok,
    failureCode: ServiceWireFrameworkErrorCode.none
  };
  const reply = {
    ...original,
    terminalResult: RequestResult.Ok,
    failureCode: ServiceWireFrameworkErrorCode.none,
    payload: {
      packetName: 'LargeResult',
      contentType: 'application/json',
      bytes: Buffer.alloc(4096, 65)
    }
  };
  const next = {
    ...entry,
    operationId: 'ffffffffffffffffffffffffffffffff',
    reply: multipart
      .encodeMultipartPayload(wire.encodeMaintenanceReplyRelay(reply))
      .toString('base64')
  };
  const record = { ...example, entries: [entry] };
  const before = structuredClone(record);
  const size =
    encodeRelocationCompletionRecord({
      ...record,
      entries: [...record.entries, next]
    }).length +
    2 * ('sourceLeaseExpired'.length - 'pending'.length);
  assert.deepEqual(
    completionStore.completionEntryWithinBlobBound(record, next, size).entry,
    next,
    'exact bound retains result'
  );
  const failed = completionStore.completionEntryWithinBlobBound(record, next, size - 1).entry;
  assert.deepEqual({ ...failed, reply: next.reply }, next, 'identity and delivery are unchanged');
  const decoded = wire.decodeMaintenanceReplyRelay(
    multipart.decodeMultipartParts(Buffer.from(failed.reply, 'base64'), (x) => x)
  );
  assert.equal(decoded.terminalResult, RequestResult.InternalError);
  assert.equal(decoded.failureCode, ServiceWireFrameworkErrorCode.requestFailed);
  assert.equal(decoded.payload, undefined);
  assert.deepEqual(
    { ...decoded, terminalResult: original.terminalResult, failureCode: original.failureCode },
    original
  );
  assert.deepEqual(record, before);
  assert.ok(
    encodeRelocationCompletionRecord({ ...record, entries: [...record.entries, failed] }).length <
      size - 1
  );
});

const fullU64 = 18446744073709551615n;
const longestDelivery = 'sourceLeaseExpired';

function entryAtWireLimit(index, payloadBytes = 0) {
  const operationId = 'f'.repeat(31) + index;
  const relay = {
    relocation: { high: 1n, low: 2n },
    targetAttemptGeneration: 1n,
    operation: { high: fullU64, low: BigInt('0x' + operationId.slice(16)) },
    replyRouteId: fullU64,
    participantId: 1n,
    sequence: BigInt(index),
    coordinator: {
      ownerId: 'x'.repeat(255),
      leaseGeneration: 1n,
      nodeRid: 'n'.repeat(255),
      nodeGeneration: 1n,
      expectedAuthorityStoreVersion: 'v'.repeat(4096)
    },
    terminalResult: payloadBytes ? RequestResult.Ok : RequestResult.InternalError,
    failureCode: payloadBytes
      ? ServiceWireFrameworkErrorCode.none
      : ServiceWireFrameworkErrorCode.requestFailed,
    ...(payloadBytes && {
      payload: {
        packetName: 'Result',
        contentType: 'application/json',
        bytes: Buffer.alloc(payloadBytes, 65)
      }
    })
  };
  return {
    ...entry,
    operationId,
    source: {
      ownerId: '\x01'.repeat(255),
      leaseGeneration: fullU64.toString(),
      nodeGeneration: fullU64.toString(),
      nodeRid: 'aa'.repeat(255)
    },
    replyRouteId: fullU64.toString(),
    reply: multipart
      .encodeMultipartPayload(wire.encodeMaintenanceReplyRelay(relay))
      .toString('base64'),
    delivery: 'pending'
  };
}

function longestSize(entries) {
  return encodeRelocationCompletionRecord({
    ...example,
    entries: entries.map((value) => ({ ...value, delivery: longestDelivery }))
  }).length;
}

function setBlobBound(t, bound) {
  const real = completionStore.COMPLETION_MAX_BYTES;
  completionStore.COMPLETION_MAX_BYTES = bound;
  t.after(() => {
    completionStore.COMPLETION_MAX_BYTES = real;
  });
}

function targetAtCutover(f, entries) {
  const history = [];
  const source = { ...f.current(), ownerId: 'source' };
  let authority = source;
  const stage = {
    phase: 'ready',
    cutoverReceived: true,
    boundaryRelay: [],
    offer: { prepare: { target: {} }, reservation: { prepared: { fence: {}, plan: {} } } },
    staging: {
      // The production field comes from the captured inventory.
      envelope: { aggregateId: example.relocationId },
      primaryAuthorityKey: f.key,
      hidden: new Map([
        [f.key.value, { replayPackets: entries.map(() => ({ returnResponse: true })) }]
      ])
    },
    owner: {
      async abort() {
        history.push('abort staging');
      }
    },
    releaseActivation() {},
    releaseJoinAdmission() {}
  };
  const runtime = new ZLinkHostServiceRelocationRuntime({
    locationStore: () => ({
      async abortAggregate() {
        history.push('abort reservation');
        return { kind: 'aborted' };
      },
      async commitAggregate(fence) {
        history.push('commit');
        assert.equal(fence.acceptedRequests.get(f.key.value), BigInt(entries.length));
        authority = f.current();
        return { kind: 'committed' };
      }
    })
  });
  runtime.readAggregateForCommitRetry = async () => ({
    kind: 'committed',
    authorities: new Map([[f.key.value, authority]])
  });
  return { runtime, stage, history, source, authority: () => authority };
}

test('target aborts before changing source authority when failure replies cannot all fit', async (t) => {
  const entries = [entryAtWireLimit(1), entryAtWireLimit(2), entryAtWireLimit(3)];
  setBlobBound(t, longestSize(entries.slice(0, 2)) + 1);
  const f = completionFixture(3n);
  const target = targetAtCutover(f, entries);
  await assert.rejects(target.runtime.finalizeTargetStage('mesh', 'staging', target.stage), {
    kind: ZLinkFrameworkErrorKind.InternalFailure
  });
  assert.deepEqual(target.history, ['abort reservation', 'abort staging']);
  assert.equal(target.authority(), target.source);
});

test('one byte below the bound commits and stores every result and longest delivery', async (t) => {
  const entries = [entryAtWireLimit(1), entryAtWireLimit(2)];
  setBlobBound(t, longestSize(entries) + 1);
  const f = completionFixture(2n);
  const target = targetAtCutover(f, entries);
  const committed = await target.runtime.commitTargetReservation(
    target.stage,
    target.stage.offer.reservation
  );
  assert.equal(committed, f.current());
  assert.deepEqual(target.history, ['commit']);
  for (const result of entries) {
    const published = await f.owner.append(f.key, f.current(), result);
    await f.owner.acknowledge(f.key, published, result, longestDelivery);
  }
  assert.deepEqual(
    (await f.owner.read(f.current())).entries,
    entries.map((value) => ({ ...value, delivery: longestDelivery }))
  );
  f.owner.requireCompleted(f.current());
});

test('append substitutes a failure to preserve space for the next request and longest ACKs', async (t) => {
  const first = entryAtWireLimit(1);
  const second = entryAtWireLimit(2);
  const success = entryAtWireLimit(1, 512);
  const bound = longestSize([first, second]);
  setBlobBound(t, bound);
  assert.ok(encodeRelocationCompletionRecord({ ...example, entries: [success] }).length < bound);
  assert.ok(longestSize([success, second]) > bound);
  const f = completionFixture(2n);
  const published = await f.owner.append(f.key, f.current(), success);
  assert.deepEqual((await f.owner.read(published)).entries, [first]);
  const acknowledged = await f.owner.acknowledge(f.key, published, first, longestDelivery);
  assert.equal(await f.owner.append(f.key, acknowledged, success), acknowledged);
  const next = await f.owner.append(f.key, acknowledged, second);
  const terminal = await f.owner.acknowledge(f.key, next, second, longestDelivery);
  assert.equal(longestSize((await f.owner.read(terminal)).entries), bound);
  f.owner.requireCompleted(terminal);
});
