'use strict';

const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const test = require('node:test');

const nodeRoot = path.resolve(__dirname, '../..');
const e2eRoot = path.join(nodeRoot, 'e2e/ObservabilityOps');

const scenarios = [
  'obs-a1-flow-correlation-scenario.ts',
  'obs-a2-error-flow-scenario.ts',
  'obs-a3-flow-propagation-scenario.ts',
  'obs-a4-fanout-timer-scenario.ts',
  'obs-b1-connection-metrics-scenario.ts',
  'obs-b2-queue-transfer-metrics-scenario.ts',
  'obs-b3-fanout-lease-metrics-scenario.ts',
  'obs-b4-disabled-metrics-scenario.ts',
  'obs-c1-draining-marker-scenario.ts',
  'obs-c2-actor-handoff-scenario.ts',
  'obs-c3-spot-drain-policies-scenario.ts',
  'obs-c4-forced-session-drain-scenario.ts',
  'obs-c5-rollout-scenario.ts'
];

test('Config 11 owns independent role apps and one client scenario per contract ID', () => {
  for (const role of ['Play', 'Session', 'Workflow']) {
    assert(fs.existsSync(path.join(e2eRoot, 'Server', role, 'main.ts')), `missing ${role} role app`);
    assert(fs.existsSync(path.join(e2eRoot, 'Server', role, 'package.json')), `missing ${role} role package`);
  }
  assert(fs.existsSync(path.join(e2eRoot, 'Client/main.ts')), 'missing trigger client app');
  for (const scenario of scenarios) {
    assert(fs.existsSync(path.join(e2eRoot, 'Client/Scenarios', scenario)), `missing ${scenario}`);
  }
});

test('Config 11 runner executes its own deployment instead of synthesizing PASS', () => {
  const runner = fs.readFileSync(path.join(e2eRoot, 'run_e2e.sh'), 'utf8');
  assert.doesNotMatch(runner, /e2e\/(AutomaticTurnDispatch|SpotActorTransfer)\/run_e2e\.sh/);
  assert.doesNotMatch(runner, /test\/contract\/(runtime-metrics|drain-control)\.test\.js/);
  assert.doesNotMatch(runner, /echo\s+"\$scenario[^\n]*PASS/);
  assert.match(runner, /Server\/Play/);
  assert.match(runner, /Server\/Session/);
  assert.match(runner, /Server\/Workflow/);
  assert.match(runner, /Client\/main/);
});
