const assert = require('node:assert/strict');
const test = require('node:test');

process.env.ZLINK_NODE_STRUCTURAL_GUARD = '1';
const {
  ZLinkStateLane,
  debugPendingWorkNames
} = require('../../packages/framework/dist/runtime/execution/state-lane');
const { ZLinkSerialExecutionQueue } = require('../../packages/framework/dist/runtime/execution/serial-execution-queue');
const { DefaultZLinkSessionContext } = require('../../packages/framework/dist/runtime/streams/session-context');
const { ZLinkActorRuntimeState } = require('../../packages/framework/dist/runtime/actors/actor-runtime-state');

test('debug guard rejects a state lane waiting on another state lane through a helper', async () => {
  const outer = new ZLinkStateLane();
  const inner = new ZLinkStateLane();
  const waitForInner = async () => await inner.run(() => 42);

  await assert.rejects(
    outer.run(async () => await waitForInner()),
    /state lane.*another state lane/u
  );
  await Promise.all([outer.dispose(), inner.dispose()]);
});

test('debug guard follows a completion through a catch chain', async () => {
  const outer = new ZLinkStateLane();
  const inner = new ZLinkStateLane();

  await assert.rejects(
    outer.run(async () => await inner.run(() => 42).catch(() => 0)),
    /state lane.*another state lane/u
  );
  await Promise.all([outer.dispose(), inner.dispose()]);
});

test('debug guard permits a detached state lane submission', async () => {
  const outer = new ZLinkStateLane();
  const inner = new ZLinkStateLane();
  let completion;

  await outer.run(() => {
    completion = inner.run(() => 42);
  });
  assert.equal(await completion, 42);
  await Promise.all([outer.dispose(), inner.dispose()]);
});

test('debug guard ignores a completed turn retained by an asynchronous callback', async () => {
  const outer = new ZLinkStateLane();
  const inner = new ZLinkStateLane();
  let release;
  const ready = new Promise((resolve) => { release = resolve; });
  let followup;

  await outer.run(() => {
    followup = ready.then(async () => await inner.run(() => 42));
  });
  release();
  assert.equal(await followup, 42);
  await Promise.all([outer.dispose(), inner.dispose()]);
});

test('debug guard ignores an asynchronous result returned without waiting for it', async () => {
  const outer = new ZLinkStateLane();
  const inner = new ZLinkStateLane();
  const result = await outer.run(() => ({
    completion: (async () => await inner.run(() => 42))()
  }));

  assert.equal(await result.completion, 42);
  await Promise.all([outer.dispose(), inner.dispose()]);
});

test('debug guard rejects a state lane waiting on serial completion through a helper', async () => {
  const lane = new ZLinkStateLane();
  const serial = new ZLinkSerialExecutionQueue(async (record) => {
    try {
      record.resolve(await record.operation());
    } finally {
      record.release();
    }
  });
  const submit = async () => await serial.submit(() => 42);

  await assert.rejects(
    lane.run(async () => await submit()),
    /state lane.*serial completion/u
  );
  await lane.dispose();
  await serial.whenIdle();
});

test('debug drain inventory names pending state and serial work until terminal', async () => {
  let releaseState;
  const stateTerminal = new Promise((resolve) => { releaseState = resolve; });
  const lane = new ZLinkStateLane();
  const stateCompletion = lane.run(() => stateTerminal);

  let releaseSerial;
  const serialTerminal = new Promise((resolve) => { releaseSerial = resolve; });
  const serial = new ZLinkSerialExecutionQueue(async (record) => {
    try {
      record.resolve(await record.operation());
    } finally {
      record.release();
    }
  });
  const serialCompletion = serial.submit(() => serialTerminal, { lane: 'lifecycle' });

  const pending = debugPendingWorkNames().join('\n');
  assert.match(pending, /state lane.*serial lifecycle/su);
  assert.match(pending, /state-lane-structure-guard\.test\.js/u);
  releaseState();
  releaseSerial();
  await Promise.all([stateCompletion, serialCompletion, lane.dispose(), serial.whenIdle()]);
  assert.deepEqual(debugPendingWorkNames(), []);
});

test('debug guard rejects a lane waiting on bound Session transport completion', async () => {
  const context = Object.create(DefaultZLinkSessionContext.prototype);
  context.stream = {
    enqueueActorBound: async () => undefined,
    enqueueActorUnbound: async () => undefined
  };
  const lane = new ZLinkStateLane();

  await assert.rejects(
    lane.run(async () => await context.actorSlotControls.enqueueBound(1, 'actor')),
    /state lane.*binding transport completion/u
  );
  await lane.dispose();
});

test('debug guard rejects a bound Session target installed outside its owner method', () => {
  const actor = new ZLinkActorRuntimeState('actor');
  assert.throws(
    () => { actor.remoteBoundSessionTargetValue = { sessionRid: 'session' }; },
    /must be installed by installBoundSessionBinding/u
  );
});
