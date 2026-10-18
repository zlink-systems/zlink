const assert = require('node:assert/strict');
const test = require('node:test');
const {
  ZLinkHostServiceRelocationRuntime
} = require('../../packages/framework/dist/runtime/host/service-relocation-host-runtime');

function target() {
  const runtime = Object.create(ZLinkHostServiceRelocationRuntime.prototype);
  const events = [];
  runtime.options = {};
  runtime.terminalTargets = { remember() {} };
  runtime.targetStages = new Map();
  runtime.targetAssemblies = new Map();
  runtime.commitTargetReservation = async () => {
    events.push('committed');
    return { storeVersion: { value: '1' } };
  };
  runtime.finalizeActorJoinProfiles = async () => events.push('completion');
  runtime.clearTargetRelocationPublication = async (_stage, authority) => {
    events.push('normalized');
    return authority;
  };
  runtime.publishSessionRoutes = async () => events.push('command44');
  runtime.targetReplyRelayCoordinator = () => ({});
  const stage = {
    phase: 'ready',
    releaseActivation() {},
    releaseJoinAdmission() {},
    offer: { reservation: {}, prepareFingerprint: 'same' },
    staging: { hidden: new Map(), envelope: { participants: [] } },
    owner: {
      async normalize() {},
      async publish() {},
      async openAdmission() {
        events.push('handler');
      }
    }
  };
  return { runtime, stage, events };
}

async function finalize(runtime, stage) {
  try {
    await runtime.finalizeTargetStage('mesh', 'move-1', stage);
  } catch (error) {
    throw error.cause ?? error;
  }
}

test('the previous target move submits command 44 before its first handler starts the next Join', async () => {
  const { runtime, stage, events } = target();
  stage.owner.openAdmission = async () => {
    events.push('handler');
    assert.equal(stage.phase, 'open');
    assert.ok(events.includes('command44'), 'previous Session route must precede the next Join');
    events.push('next-join');
  };
  await finalize(runtime, stage);
  assert.ok(events.indexOf('committed') < events.indexOf('handler'));
});

test('target normalization precedes Join completion and cannot overwrite the next root', async () => {
  const { runtime, stage, events } = target();
  let root = 'first-root';
  runtime.clearTargetRelocationPublication = async (_stage, authority) => {
    assert.equal(root, 'first-root', 'normalization must use the previous committed root');
    events.push('normalized');
    root = 'steady';
    return authority;
  };
  runtime.finalizeActorJoinProfiles = async () => {
    assert.ok(events.includes('normalized'), 'normalization must precede OnJoinCompleted');
    root = 'second-root';
  };
  await finalize(runtime, stage);
  await finalize(runtime, stage);
  assert.equal(root, 'second-root');
  assert.equal(events.filter((event) => event === 'normalized').length, 1);
});

test('request terminal relay uses the normalized authority StoreVersion', async () => {
  const { runtime, stage } = target();
  const packet = { index: 0, returnResponse: true, source: { replyRouteId: '1' } };
  const hidden = { actor: { context: { actorId: 'actor' } }, replayPackets: [packet] };
  stage.staging.hidden.set('actor', hidden);
  stage.staging.envelope.participants.push({ key: 'actor' });
  stage.finalize = Promise.resolve();
  runtime.clearTargetRelocationPublication = async () => ({
    storeVersion: { value: 'normalized-v2' }
  });
  runtime.targetReplyRelayCoordinator = (_mesh, _stage, authority) => ({
    expectedAuthorityStoreVersion: authority.storeVersion.value
  });
  const observed = [];
  runtime.relayTerminalReply = async (_mesh, _stage, coordinator) =>
    observed.push(coordinator.expectedAuthorityStoreVersion);
  stage.owner.normalize = async () => {
    await hidden.onReplayResult(packet, { index: 0, ok: true, response: 'reply' });
  };
  await finalize(runtime, stage);
  assert.deepEqual(observed, ['normalized-v2']);
});
