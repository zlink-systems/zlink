import assert from 'node:assert/strict';
import { readFile } from 'node:fs/promises';
import { test } from 'node:test';

import {
  SERVICE_WIRE_MAGIC,
  SERVICE_WIRE_MAJOR,
  ServiceWireCommand
} from '../../../../runtime/protocol/generated/node/service_wire_constants';
import { ZLinkNodeRawBindingPort } from '../../packages/framework/src/runtime/backend/node/node-raw-binding-port';
import {
  EventLoopResourceStack,
  EventLoopWorkQueues
} from '../../packages/framework/src/runtime/foundation/event-loop-resources';
import {
  OperationCapacityExceededError,
  OperationCancelledError,
  OperationRegistry,
  OperationTimeoutError,
  type OperationClock
} from '../../packages/framework/src/runtime/foundation/operation-registry';
import {
  createServiceWireCodec,
  ServiceWireDecodeError
} from '../../packages/framework/src/runtime/foundation/service-wire-codec';

const wireCodec = createServiceWireCodec({
  magic: SERVICE_WIRE_MAGIC,
  major: SERVICE_WIRE_MAJOR,
  commands: ServiceWireCommand
});

interface Fixture {
  readonly probeId: string;
  readonly canonical: ReadonlyArray<{ commandId: number; bytes: number[] }>;
  readonly malformed: ReadonlyArray<{ bytes: number[]; error: string }>;
}

test('transport gateway uses only the public binding package and retains received bytes', async () => {
  const host = new ZLinkNodeRawBindingPort().createHost();
  const router = host.createRouter();
  const dealer = host.createDealer();
  const endpoint = `inproc://m5-foundation-${process.pid}-${Date.now()}`;
  try {
    router.bind(endpoint);
    dealer.setRoutingId('m5-dealer');
    dealer.connect(endpoint);
    assert.equal(dealer.send([Buffer.from('foundation')]), true);
    const received = await pollReceive(() => router.receive(true));
    assert.equal(received.sourceRid, 'm5-dealer');
    assert.deepEqual(received.parts.map(part => part.toString()), ['foundation']);
  } finally {
    host.close();
  }
});

test('codec consumes canonical and malformed shared fixtures', async () => {
  const fixture = JSON.parse(
    await readFile('../../runtime/protocol/golden/service-decoder-fixtures-v1.json', 'utf8')
  ) as Fixture;
  for (const item of fixture.canonical) {
    const decoded = wireCodec.decodeLivenessRecord(Uint8Array.from(item.bytes));
    assert.equal(decoded.command, item.commandId);
    assert.equal(decoded.probeId, BigInt(fixture.probeId));
    assert.deepEqual([...wireCodec.encodeLivenessRecord(decoded)], item.bytes);
  }
  for (const item of fixture.malformed) {
    assert.throws(
      () => wireCodec.decodeLivenessRecord(Uint8Array.from(item.bytes)),
      (error: unknown) => error instanceof ServiceWireDecodeError && error.code === item.error
    );
  }
  assert.equal(ServiceWireCommand.livenessProbe, fixture.canonical[0]?.commandId);
  assert.equal(ServiceWireCommand.livenessAck, fixture.canonical[1]?.commandId);
});

class ManualClock implements OperationClock {
  readonly callbacks = new Map<object, () => void>();
  setTimeout(callback: () => void): object {
    const handle = {};
    this.callbacks.set(handle, callback);
    return handle;
  }
  clearTimeout(handle: unknown): void {
    this.callbacks.delete(handle as object);
  }
  fire(): void {
    const callbacks = [...this.callbacks.values()];
    for (const callback of callbacks) callback();
  }
}

test('Promise completion is terminal once across reply, timeout, and shutdown', async () => {
  const clock = new ManualClock();
  const operations = new OperationRegistry<string>(clock);
  const completed = operations.reserve(100);
  assert.equal(operations.complete(completed.id, 'reply'), true);
  clock.fire();
  assert.equal(operations.complete(completed.id, 'late'), false);
  assert.equal(await completed.promise, 'reply');

  const timedOut = operations.reserve(100);
  clock.fire();
  await assert.rejects(timedOut.promise, OperationTimeoutError);
  assert.equal(operations.cancel(timedOut.id), false);

  const cancelled = operations.reserve(100);
  operations.close();
  await assert.rejects(cancelled.promise, OperationCancelledError);
  assert.equal(operations.size, 0);
});

test('operation registry rejects work before allocating beyond its capacity', async () => {
  const clock = new ManualClock();
  const operations = new OperationRegistry<string>(clock, 1);
  const pending = operations.reserve(100);
  assert.throws(
    () => operations.reserve(100),
    OperationCapacityExceededError
  );
  assert.equal(operations.size, 1);
  operations.complete(pending.id, 'reply');
  assert.equal(await pending.promise, 'reply');
  assert.equal(operations.size, 0);
});

test('event-loop resources close in reverse once and infrastructure remains independent', async () => {
  const order: string[] = [];
  const resources = new EventLoopResourceStack();
  resources.own({ close: () => void order.push('first') });
  resources.own({ close: async () => void order.push('second') });
  await Promise.all([resources.close(), resources.close()]);
  assert.deepEqual(order, ['second', 'first']);
  assert.throws(() => resources.own({ close() {} }), /closing/);

  const queues = new EventLoopWorkQueues(1, 1);
  let release!: () => void;
  const blocked = new Promise<void>(resolve => (release = resolve));
  assert.equal(queues.submitApplication(() => blocked), true);
  assert.equal(queues.submitApplication(() => undefined), false);
  const infrastructureDone = new Promise<void>(resolve => {
    assert.equal(queues.submitInfrastructure(resolve), true);
  });
  await infrastructureDone;
  release();
  await new Promise(resolve => setImmediate(resolve));
  queues.stopAdmission();
  assert.equal(queues.submitInfrastructure(() => undefined), false);
});

async function pollReceive<T>(receive: () => T | undefined): Promise<T> {
  const deadline = Date.now() + 1_000;
  while (Date.now() < deadline) {
    const value = receive();
    if (value !== undefined) return value;
    await new Promise(resolve => setImmediate(resolve));
  }
  throw new Error('Timed out waiting for raw transport input.');
}
