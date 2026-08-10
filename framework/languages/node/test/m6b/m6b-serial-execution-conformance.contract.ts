import assert from 'node:assert/strict';
import { existsSync, readFileSync } from 'node:fs';
import { dirname, join, parse } from 'node:path';
import { test } from 'node:test';

import {
  ZLinkFrameworkErrorKind,
  ZLinkFrameworkException
} from '../../packages/framework/src/contracts';
import {
  DefaultZLinkActorClient,
  ZLinkActorDispatchMailboxSet,
  ZLinkActorPacketKind,
  ZLinkSpotActorDispatcher,
  ZLinkSpotActorHandlerRegistryRuntime
} from '../../packages/framework/src/runtime/actors';
import {
  ZLinkExecutionBarrier
} from '../../packages/framework/src/runtime/execution';
import {
  ZLINK_DEFAULT_SERIAL_SCHEDULER_OPTIONS,
  ZLinkBoundedSerialScheduler,
  type ZLinkSerialSchedulerOptions,
  type ZLinkSerialWorkLane,
  type ZLinkSerialWorkRecord
} from '../../packages/framework/src/runtime/execution/serial-scheduler';
import { ZLinkSpotSerialExecutor } from '../../packages/framework/src/runtime/spots/spot-serial-executor';

interface Deferred<T> {
  readonly promise: Promise<T>;
  resolve(value: T): void;
}

interface SerialExecutionFixture {
  readonly limits: {
    readonly application: { readonly messageCapacity: number; readonly byteCapacity: number };
    readonly lifecycle: { readonly messageCapacity: number; readonly byteCapacity: number };
    readonly ownerTimeBudgetMilliseconds: number;
    readonly lifecycleBurstLimit: number;
    readonly fixedWorkByteCost: number;
  };
  readonly accountingScenarios: ReadonlyArray<{
    readonly name: string;
    readonly lane: ZLinkSerialWorkLane;
    readonly retainedPayloadBytesPerWork: number;
    readonly acceptedWorkCount: number;
    readonly nextAdmission: 'capacityExceeded';
    readonly runningWorkConsumesReservation: true;
  }>;
  readonly admissionInvariants: {
    readonly commit: readonly string[];
    readonly rejectionPreserves: readonly string[];
    readonly enqueueFailureRestoresReservation: true;
    readonly releaseOccursAfterTerminalCompletion: true;
  };
  readonly arbitrationScenarios: ReadonlyArray<{
    readonly name: string;
    readonly applicationInput: readonly string[];
    readonly lifecycleInput: readonly string[];
    readonly expectedSelection: readonly string[];
  }>;
  readonly sameOwnerCalls: ReadonlyArray<{
    readonly target: 'selfActor' | 'sameSpot' | 'differentMemberActorOnSameSpot' | 'differentOwner';
    readonly async: string;
    readonly yield: string;
    readonly actorClaimAfterYield: string;
  }>;
  readonly dispatchInvariants: {
    readonly applicationAndLifecycleUseDistinctFifos: true;
    readonly applicationAndLifecycleHaveIndependentAdmission: true;
    readonly emptyToNonEmptySchedulesImmediately: true;
    readonly pollingIsNotAProgressMechanism: true;
    readonly implicitInlineExecution: false;
    readonly resumeAfterYieldUsesNewTurn: true;
  };
}

function deferred<T>(): Deferred<T> {
  let resolve!: (value: T) => void;
  const promise = new Promise<T>((complete) => {
    resolve = complete;
  });
  return { promise, resolve };
}

function fixturePath(): string {
  let current = process.cwd();
  const root = parse(current).root;
  for (;;) {
    const candidate = join(
      current,
      'framework/runtime/conformance/serial-execution-v1.json'
    );
    if (existsSync(candidate)) return candidate;
    if (current === root) break;
    current = dirname(current);
  }
  throw new Error('serial-execution-v1.json was not found from the current workspace.');
}

const fixture = JSON.parse(readFileSync(fixturePath(), 'utf8')) as SerialExecutionFixture;

function fixtureOptions(): ZLinkSerialSchedulerOptions {
  return {
    applicationMessageCapacity: fixture.limits.application.messageCapacity,
    applicationByteCapacity: fixture.limits.application.byteCapacity,
    lifecycleMessageCapacity: fixture.limits.lifecycle.messageCapacity,
    lifecycleByteCapacity: fixture.limits.lifecycle.byteCapacity,
    ownerTimeBudgetMs: fixture.limits.ownerTimeBudgetMilliseconds,
    lifecycleBurstLimit: fixture.limits.lifecycleBurstLimit,
    fixedWorkByteCost: fixture.limits.fixedWorkByteCost
  };
}

function scheduler(
  acceptedSequences: bigint[] = [],
  options: ZLinkSerialSchedulerOptions = fixtureOptions()
): ZLinkBoundedSerialScheduler {
  return new ZLinkBoundedSerialScheduler(async (record) => {
    acceptedSequences.push(record.acceptedSequence);
    try {
      record.resolve(await record.operation());
    } catch (error) {
      record.reject(error);
    } finally {
      record.release();
    }
  }, options);
}

function laneSnapshot(
  snapshot: ReturnType<ZLinkBoundedSerialScheduler['snapshot']>,
  lane: ZLinkSerialWorkLane
): { readonly messages: number; readonly bytes: number } {
  return lane === 'application'
    ? { messages: snapshot.applicationMessages, bytes: snapshot.applicationBytes }
    : { messages: snapshot.lifecycleMessages, bytes: snapshot.lifecycleBytes };
}

test('Node serial defaults are read directly from the shared conformance fixture', () => {
  assert.deepEqual(ZLINK_DEFAULT_SERIAL_SCHEDULER_OPTIONS, {
    applicationMessageCapacity: fixture.limits.application.messageCapacity,
    applicationByteCapacity: fixture.limits.application.byteCapacity,
    lifecycleMessageCapacity: fixture.limits.lifecycle.messageCapacity,
    lifecycleByteCapacity: fixture.limits.lifecycle.byteCapacity,
    ownerTimeBudgetMs: fixture.limits.ownerTimeBudgetMilliseconds,
    lifecycleBurstLimit: fixture.limits.lifecycleBurstLimit,
    fixedWorkByteCost: fixture.limits.fixedWorkByteCost
  });
});

test('fixture count and byte boundaries retain running reservations through terminal completion', async () => {
  for (const scenario of fixture.accountingScenarios) {
    const serial = scheduler();
    const started = deferred<void>();
    const terminal = deferred<void>();
    const workOptions = {
      lane: scenario.lane,
      payloadBytes: scenario.retainedPayloadBytesPerWork
    } as const;
    const admitted: Promise<void>[] = [serial.submit(async () => {
      started.resolve();
      await terminal.promise;
    }, workOptions)];
    await started.promise;
    for (let index = 1; index < scenario.acceptedWorkCount; index += 1) {
      admitted.push(serial.submit(() => undefined, workOptions));
    }

    const retained = laneSnapshot(serial.snapshot(), scenario.lane);
    assert.equal(retained.messages, scenario.acceptedWorkCount, scenario.name);
    assert.equal(
      retained.bytes,
      (scenario.retainedPayloadBytesPerWork + fixture.limits.fixedWorkByteCost)
        * scenario.acceptedWorkCount,
      scenario.name
    );
    await assert.rejects(serial.submit(() => undefined, workOptions), /queue is full/u);
    assert.deepEqual(laneSnapshot(serial.snapshot(), scenario.lane), retained, scenario.name);

    terminal.resolve();
    await Promise.all(admitted);
    assert.deepEqual(laneSnapshot(serial.snapshot(), scenario.lane), {
      messages: 0,
      bytes: 0
    }, scenario.name);
  }
});

test('fixture arbitration uses two FIFO lanes on one event loop', async () => {
  const scenario = fixture.arbitrationScenarios.find(
    ({ name }) => name === 'lifecycle-debt-yields-to-application'
  );
  assert.ok(scenario);
  const selected: string[] = [];
  const serial = scheduler();
  const jobs = [
    ...scenario.applicationInput.map(value => serial.submit(
      () => { selected.push(value); },
      { lane: 'application' }
    )),
    ...scenario.lifecycleInput.map(value => serial.submit(
      () => { selected.push(value); },
      { lane: 'lifecycle' }
    ))
  ];
  await Promise.all(jobs);
  assert.deepEqual(selected, scenario.expectedSelection);
});

test('fixture lanes admit independently and an empty queue schedules without polling', async () => {
  assert.equal(fixture.dispatchInvariants.applicationAndLifecycleUseDistinctFifos, true);
  assert.equal(fixture.dispatchInvariants.applicationAndLifecycleHaveIndependentAdmission, true);
  assert.equal(fixture.dispatchInvariants.emptyToNonEmptySchedulesImmediately, true);
  assert.equal(fixture.dispatchInvariants.pollingIsNotAProgressMechanism, true);

  const serial = scheduler();
  const applicationGate = deferred<void>();
  const applicationJobs = Array.from(
    { length: fixture.limits.application.messageCapacity },
    () => serial.submit(() => applicationGate.promise, { lane: 'application' })
  );
  const lifecycle = serial.submit(() => 'lifecycle', { lane: 'lifecycle' });
  assert.equal(await lifecycle, 'lifecycle');
  assert.deepEqual(serial.snapshot(), {
    applicationMessages: fixture.limits.application.messageCapacity,
    applicationBytes:
      fixture.limits.application.messageCapacity * fixture.limits.fixedWorkByteCost,
    lifecycleMessages: 0,
    lifecycleBytes: 0
  });
  await assert.rejects(
    serial.submit(() => undefined, { lane: 'application' }),
    /application execution queue is full/u
  );
  applicationGate.resolve();
  await Promise.all(applicationJobs);

  const immediate: string[] = [];
  const immediatelyScheduled = serial.submit(() => { immediate.push('run'); });
  assert.deepEqual(immediate, []);
  await Promise.resolve();
  assert.deepEqual(immediate, ['run']);
  await immediatelyScheduled;
});

test('append failure rolls back reservation and accepted sequence without a test bypass', async () => {
  assert.equal(fixture.admissionInvariants.enqueueFailureRestoresReservation, true);
  const sequences: bigint[] = [];
  const serial = scheduler(sequences);
  const rejectedOperation = () => 'not-visible';
  const arrayPrototype = Array.prototype as unknown as {
    push(this: unknown[], ...items: unknown[]): number;
  };
  const originalPush = arrayPrototype.push;
  arrayPrototype.push = function push(this: unknown[], ...items: unknown[]): number {
    if (items.some((item) =>
      typeof item === 'object'
      && item !== null
      && (item as Partial<ZLinkSerialWorkRecord<unknown>>).operation === rejectedOperation)) {
      throw new Error('injected FIFO append failure');
    }
    return originalPush.apply(this, items);
  };
  try {
    await assert.rejects(serial.submit(rejectedOperation), /injected FIFO append failure/u);
  } finally {
    arrayPrototype.push = originalPush;
  }
  assert.deepEqual(serial.snapshot(), {
    applicationMessages: 0,
    applicationBytes: 0,
    lifecycleMessages: 0,
    lifecycleBytes: 0
  });

  await serial.submit(() => undefined);
  await serial.submit(() => undefined);
  assert.deepEqual(sequences, [1n, 2n]);
});

test('append failure releases the production execution-barrier authority claim', async () => {
  const barrier = new ZLinkExecutionBarrier();
  const serial = new ZLinkSpotSerialExecutor(true);
  serial.setExecutionBarrier(barrier);
  const rejectedOperation = () => undefined;
  const arrayPrototype = Array.prototype as unknown as {
    push(this: unknown[], ...items: unknown[]): number;
  };
  const originalPush = arrayPrototype.push;
  arrayPrototype.push = function push(this: unknown[], ...items: unknown[]): number {
    if (items.some((item) =>
      typeof item === 'object'
      && item !== null
      && (item as Partial<ZLinkSerialWorkRecord<unknown>>).operation === rejectedOperation)) {
      throw new Error('injected FIFO append failure');
    }
    return originalPush.apply(this, items);
  };
  try {
    await assert.rejects(serial.post(rejectedOperation), /injected FIFO append failure/u);
  } finally {
    arrayPrototype.push = originalPush;
  }

  const seal = barrier.seal();
  await barrier.waitForQuiescence(seal);
  assert.equal(barrier.abort(seal), true);
});

test('Actor self waits reject and same-Spot member Yield resumes on a new turn', async () => {
  assert.equal(fixture.dispatchInvariants.implicitInlineExecution, false);
  assert.equal(fixture.dispatchInvariants.resumeAfterYieldUsesNewTurn, true);
  const byTarget = new Map(fixture.sameOwnerCalls.map(value => [value.target, value]));
  assert.equal(byTarget.get('selfActor')?.async, 'invalidOperation');
  assert.equal(byTarget.get('selfActor')?.yield, 'invalidOperation');
  assert.equal(byTarget.get('differentMemberActorOnSameSpot')?.async, 'invalidOperation');
  assert.equal(byTarget.get('differentMemberActorOnSameSpot')?.yield, 'resumeOnNewTurn');
  assert.equal(byTarget.get('differentMemberActorOnSameSpot')?.actorClaimAfterYield, 'retained');

  const events: string[] = [];
  const serial = new ZLinkSpotSerialExecutor(true, 'spot-a' as never);
  const actorClaims = new ZLinkActorDispatchMailboxSet('spot-a');
  const actor = (actorId: string) => ({
    context: {
      actorId,
      objectGeneration: 1n,
      meshName: 'mesh-a',
      spotId: 'spot-a'
    }
  }) as never;
  const sourceActor = actor('actor-a');
  const targetActor = actor('actor-b');
  let dispatcher!: ZLinkSpotActorDispatcher;
  let resumedTurn = 0;
  let initialTurn = 0;
  let targetTurn = 0;
  let routeResolutions = 0;
  let transportAdmissions = 0;
  let handoffCaptures = 0;
  class SelfRequest {}
  class TargetRequest {}
  const invalidOperation = (error: unknown) => error instanceof ZLinkFrameworkException
    && error.kind === ZLinkFrameworkErrorKind.InvalidOperation;

  const client = new DefaultZLinkActorClient({
    nodeProvider: () => undefined,
    completionTableProvider: () => undefined,
    locationResolver: () => ({
      async resolveDirectActorRoute(actorId: string) {
        routeResolutions += 1;
        return {
          meshName: 'mesh-a',
          actorId,
          actorType: 'TestActor',
          actorRef: {
            actorId,
            objectGeneration: 1n,
            meshName: 'mesh-a',
            nodeRid: 'node-a'
          },
          ownerNodeRid: 'node-a',
          ownerNodeGeneration: 1n,
          spotKind: 1,
          spotId: 'spot-a',
          spotGeneration: 1n,
          membershipEpoch: 1n,
          ownerId: 'owner-a',
          ownerLeaseGeneration: 1n,
          authorityOwnerGeneration: 1n,
          authorityStoreVersion: 'version-a',
          updatedAt: new Date()
        } as never;
      },
      invalidateActorRoute() {}
    } as never),
    transportDeliveryGate: () => ({
      async waitBeforeSubmit() {
        transportAdmissions += 1;
      }
    }),
    handoffCapture: (_meshName, actorId) => {
      handoffCaptures += 1;
      return actorId === 'actor-b'
        ? actorClaims.submit('actor-b', () =>
            dispatcher.dispatchRequest(targetActor, 'TargetRequest', {}))
        : undefined;
    }
  });

  class SourceHandler {
    async handle(): Promise<string> {
      initialTurn = serial.activeTurnId;
      events.push('source:start');
      assert.throws(
        () => client.requestToActor('actor-a', new SelfRequest()).submit(),
        invalidOperation
      );
      assert.throws(
        () => client.requestToActor('actor-a', new SelfRequest()).yield(),
        invalidOperation
      );
      await assert.rejects(
        client.requestToActor('actor-b', new TargetRequest()).submit(),
        invalidOperation
      );
      assert.equal(routeResolutions, 1);
      assert.equal(transportAdmissions, 0);
      assert.equal(handoffCaptures, 0);
      assert.equal(serial.activeTurnId, initialTurn);
      events.push('same-spot:async-rejected');
      const reply = await client
        .requestToActor('actor-b', new TargetRequest())
        .yield<string>();
      resumedTurn = serial.activeTurnId;
      assert.equal(serial.isCurrentTurn, true);
      events.push(`source:resume:${reply}`);
      return reply;
    }
  }
  class TargetHandler {
    async handle(): Promise<string> {
      targetTurn = serial.activeTurnId;
      events.push('target');
      return 'pong';
    }
  }
  const registry = new ZLinkSpotActorHandlerRegistryRuntime()
    .addPacket({
      kind: ZLinkActorPacketKind.Request,
      packetName: 'source',
      actorType: Object as never,
      handlerType: SourceHandler
    })
    .addPacket({
      kind: ZLinkActorPacketKind.Request,
      packetName: 'TargetRequest',
      actorType: Object as never,
      handlerType: TargetHandler
    });
  dispatcher = new ZLinkSpotActorDispatcher({
    registry,
    spot: {} as never,
    serial
  });
  const source = actorClaims.submit('actor-a', () =>
    dispatcher.dispatchRequest<unknown, string>(sourceActor, 'source', {}));
  const next = actorClaims.submit('actor-a', () => {
    events.push('source:next');
  });

  assert.equal(await source, 'pong');
  await next;
  assert.equal(routeResolutions, 2);
  assert.equal(transportAdmissions, 1);
  assert.equal(handoffCaptures, 1);
  assert.notEqual(targetTurn, initialTurn);
  assert.notEqual(resumedTurn, initialTurn);
  assert.deepEqual(events, [
    'source:start',
    'same-spot:async-rejected',
    'target',
    'source:resume:pong',
    'source:next'
  ]);
});
