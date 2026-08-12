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
