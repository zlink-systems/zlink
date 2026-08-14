'use strict';

const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const test = require('node:test');

const framework = require('../../packages/framework/dist/internal');
const projection = require(
  '../../packages/framework/dist/runtime/foundation/runtime-state-projections'
);

const fixture = JSON.parse(fs.readFileSync(path.resolve(
  __dirname,
  '../../../../runtime/conformance/runtime-state-v1.json'
), 'utf8'));

const states = new Map([
  ['preparing', framework.ZLinkFrameworkRuntimeState.Preparing],
  ['serving', framework.ZLinkFrameworkRuntimeState.Serving],
  ['relocating', framework.ZLinkFrameworkRuntimeState.Relocating],
  ['relocated', framework.ZLinkFrameworkRuntimeState.Relocated],
  ['draining', framework.ZLinkFrameworkRuntimeState.Draining],
  ['stopped', framework.ZLinkFrameworkRuntimeState.Stopped],
  ['error', framework.ZLinkFrameworkRuntimeState.Error]
]);

test('runtime state projections consume the shared public authority fixture', () => {
  assert.equal(fixture.fixture, 'zlink.framework.runtime-state');
  for (const entry of fixture.publicStates) {
    const state = states.get(entry.name);
    assert.equal(state, entry.wireValue, `${entry.name}:wire`);
    assert.equal(
      projection.runtimeStateIsReady(state),
      entry.isReady,
      `${entry.name}:ready`
    );
  }

  for (const scenario of fixture.acceptingWorkScenarios) {
    assert.equal(
      projection.runtimeAcceptsWork(
        states.get(scenario.state),
        scenario.admissionOpen
      ),
      scenario.expected,
      `${scenario.state}:acceptingWork`
    );
  }
});

test('runtime status exposes one host capacity snapshot and resets its epoch', async () => {
  const runtime = new framework.ZLinkFrameworkRuntimeHost({
    registration: framework.createFrameworkRegistration()
  });

  assert.equal(runtime.status.capacity.measurementEpoch, 0n);
  assert.equal(runtime.status.capacity.coreHwm.effectiveBudgetBytes, 0n);
  assert.equal(
    runtime.status.capacity.applicationJobQueue.configuredProfile,
    framework.ZLinkApplicationJobQueueProfile.Balanced
  );
  assert.throws(
    () => runtime.resetCapacityMetrics(),
    /requires a started Framework runtime/u
  );

  try {
    await runtime.start();
    const before = runtime.status.capacity;
    const nativeBefore = runtime.context.getCoreHwmBudgetSnapshot();
    assert.equal(before.measurementEpoch, nativeBefore.measurementEpoch);
    assert.equal(before.coreHwm.effectiveBudgetBytes, nativeBefore.effectiveCoreBudgetBytes);
    assert.equal(before.coreHwm.currentAccountedBytes, nativeBefore.currentAccountedBytes);
    assert.equal(
      before.applicationJobQueue.effectiveMaxQueuedApplicationJobs,
      128n * before.applicationJobQueue.effectiveProcessorCount
    );

    runtime.resetCapacityMetrics();
    const after = runtime.status.capacity;
    assert.ok(after.measurementEpoch > before.measurementEpoch);
    assert.equal(after.coreHwm.currentAccountedBytes, before.coreHwm.currentAccountedBytes);
    assert.equal(
      after.coreHwm.completionCurrentAccountedBytes,
      before.coreHwm.completionCurrentAccountedBytes
    );
    assert.equal(
      after.applicationJobQueue.permitsInUse,
      before.applicationJobQueue.permitsInUse
    );
    assert.equal(
      after.applicationJobQueue.peakPermitsInUse,
      after.applicationJobQueue.permitsInUse
    );
    assert.equal(after.applicationJobQueue.capacityWaitCount, 0n);
    assert.equal(after.applicationJobQueue.capacityWaitDurationSeconds, 0);
  } finally {
    await runtime.stop();
  }

  assert.equal(runtime.status.capacity.coreHwm.effectiveBudgetBytes, 0n);
  assert.ok(runtime.status.capacity.applicationJobQueue.effectiveMaxQueuedApplicationJobs > 0n);
});

test('maintenance and discovery are one-way bounded-context projections', () => {
  for (const [name, expected] of Object.entries(
    fixture.maintenanceAdmissionProjection
  )) {
    assert.equal(
      projection.maintenanceAdmissionState(states.get(name)),
      expected,
      `${name}:maintenance`
    );
  }
  for (const [name, expected] of Object.entries(
    fixture.discoveryAvailabilityProjection
  )) {
    if (name === 'transportOnlyState') continue;
    assert.equal(
      projection.discoveryAvailabilityForRuntimeState(states.get(name)),
      expected,
      `${name}:discovery`
    );
  }
  assert.equal(
    fixture.discoveryAvailabilityProjection.transportOnlyState,
    'disconnected'
  );
});

test('topology readiness combines host readiness with ready targets', () => {
  for (const scenario of fixture.topologyScenarios) {
    assert.equal(
      projection.topologyRuntimeIsReady(
        states.get(scenario.hostState),
        scenario.readyTargetCount
      ),
      scenario.isReady,
      `${scenario.hostState}:${scenario.readyTargetCount}`
    );
  }
});
