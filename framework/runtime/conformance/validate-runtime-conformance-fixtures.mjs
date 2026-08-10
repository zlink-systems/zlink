#!/usr/bin/env node

import assert from 'node:assert/strict';
import { readFile } from 'node:fs/promises';
import { fileURLToPath } from 'node:url';

const readFixture = async (name) => JSON.parse(await readFile(
  fileURLToPath(new URL(name, import.meta.url)),
  'utf8'
));

const fixture = await readFixture('./serial-execution-v1.json');

assert.equal(fixture.fixture, 'zlink.framework.serial-execution');
assert.equal(fixture.version, 1);

const { limits } = fixture;
assert.deepEqual(limits.application, {
  messageCapacity: 1_024,
  byteCapacity: 64 * 1_024 * 1_024
});
assert.deepEqual(limits.lifecycle, {
  messageCapacity: 128,
  byteCapacity: 4 * 1_024 * 1_024
});
assert.equal(limits.ownerTimeBudgetMilliseconds, 10);
assert.equal(limits.lifecycleBurstLimit, 8);
assert.equal(limits.fixedWorkByteCost, 256);

const uniqueNames = (records, field) => {
  const names = records.map((record) => record[field]);
  assert.equal(new Set(names).size, names.length, `${field} values must be unique`);
};

uniqueNames(fixture.accountingScenarios, 'name');
uniqueNames(fixture.arbitrationScenarios, 'name');
uniqueNames(fixture.sameOwnerCalls, 'target');

for (const scenario of fixture.accountingScenarios) {
  const lane = limits[scenario.lane];
  assert.ok(lane, `unknown lane: ${scenario.lane}`);
  assert.equal(scenario.nextAdmission, 'capacityExceeded');
  assert.equal(scenario.runningWorkConsumesReservation, true);
  const cost = limits.fixedWorkByteCost + scenario.retainedPayloadBytesPerWork;
  assert.ok(cost * scenario.acceptedWorkCount <= lane.byteCapacity);
  assert.ok(
    scenario.acceptedWorkCount === lane.messageCapacity
      || cost * (scenario.acceptedWorkCount + 1) > lane.byteCapacity,
    `${scenario.name} does not reach a count or byte boundary`
  );
}

const debt = fixture.arbitrationScenarios.find(
  ({ name }) => name === 'lifecycle-debt-yields-to-application'
);
assert.ok(debt);
assert.deepEqual(
  debt.expectedSelection.slice(0, limits.lifecycleBurstLimit),
  debt.lifecycleInput.slice(0, limits.lifecycleBurstLimit)
);
assert.equal(debt.expectedSelection[limits.lifecycleBurstLimit], debt.applicationInput[0]);

const sameOwner = Object.fromEntries(
  fixture.sameOwnerCalls.map((scenario) => [scenario.target, scenario])
);
assert.equal(sameOwner.selfActor.async, 'invalidOperation');
assert.equal(sameOwner.selfActor.yield, 'invalidOperation');
assert.equal(sameOwner.sameSpot.async, 'invalidOperation');
assert.equal(sameOwner.sameSpot.yield, 'resumeOnNewTurn');
assert.equal(sameOwner.differentMemberActorOnSameSpot.async, 'invalidOperation');
assert.equal(sameOwner.differentMemberActorOnSameSpot.yield, 'resumeOnNewTurn');

for (const [name, value] of Object.entries(fixture.admissionInvariants)) {
  assert.ok(Array.isArray(value) ? value.length > 0 : value === true, name);
}
for (const [name, value] of Object.entries(fixture.dispatchInvariants)) {
  assert.equal(value, name === 'implicitInlineExecution' ? false : true, name);
}

const observation = await readFixture('./runtime-observation-v1.json');
assert.equal(observation.fixture, 'zlink.framework.runtime-observation');
assert.equal(observation.version, 1);
assert.equal(observation.limits.defaultTerminalCapacity, 64);
assert.equal(observation.limits.signedLossCounterMaximum, '9223372036854775807');

assert.deepEqual(observation.retentionInvariants, {
  intermediateRetention: 'latestPerSource',
  terminalRetention: 'fifo',
  terminalOverflow: 'discardOldest',
  terminalOverflowClosesSubscriber: false,
  terminalReplacesSameSourceIntermediate: true,
  differentSourceIntermediateIsPreserved: true,
  sourceKeyRemoval: 'afterTerminalDeliveredOrDiscarded',
  producerInvokesSubscriber: false,
  dispatcherOwnership: 'processShared'
});
assert.deepEqual(observation.lossInvariants, {
  coalescedIntermediateCounter: 'perSubscriberSigned64Saturating',
  discardedTerminalCounter: 'perSubscriberSigned64Saturating',
  newSubscriberStartsAtZero: true,
  countersAreIndependent: true
});

uniqueNames(observation.scenarios, 'name');
const retention = observation.scenarios.find(
  ({ name }) => name === 'multi-source-retention-and-terminal-overflow'
);
assert.ok(retention);
assert.equal(retention.terminalCapacity, 2);

const latest = new Map();
const terminals = [];
const liveSources = new Set();
let coalesced = 0n;
let discarded = 0n;
for (const operation of retention.operations) {
  assert.ok(Number.isSafeInteger(operation.sequence) && operation.sequence >= 0);
  if (operation.kind === 'intermediate') {
    if (latest.has(operation.source)) coalesced += 1n;
    latest.set(operation.source, {
      sequence: operation.sequence,
      value: operation.value
    });
    liveSources.add(operation.source);
    continue;
  }
  assert.equal(operation.kind, 'terminal');
  if (latest.delete(operation.source)) coalesced += 1n;
  liveSources.add(operation.source);
  if (terminals.length === retention.terminalCapacity) {
    const removed = terminals.shift();
    liveSources.delete(removed.source);
    discarded += 1n;
  }
  terminals.push({
    source: operation.source,
    sequence: operation.sequence,
    value: operation.value
  });
}

assert.deepEqual(Object.fromEntries(latest), retention.expectedRetainedIntermediateBySource);
assert.deepEqual(terminals, retention.expectedTerminalFifo);
assert.deepEqual(
  { coalescedIntermediateCount: String(coalesced), discardedTerminalCount: String(discarded) },
  retention.expectedLoss
);
assert.deepEqual(
  [...new Set(retention.expectedRemovedSourceKeys)].sort(),
  retention.expectedRemovedSourceKeys.toSorted()
);
assert.deepEqual([...liveSources].sort(), retention.expectedRetainedSourceKeys.toSorted());
for (const source of retention.expectedRemovedSourceKeys) assert.equal(liveSources.has(source), false);

const saturation = observation.scenarios.find(
  ({ name }) => name === 'loss-counters-saturate-independently'
);
assert.ok(saturation);
const lossMaximum = BigInt(observation.limits.signedLossCounterMaximum);
const saturatingAdd = (value, increment) => {
  const sum = BigInt(value) + BigInt(increment);
  return String(sum > lossMaximum ? lossMaximum : sum);
};
assert.deepEqual({
  coalescedIntermediateCount: saturatingAdd(
    saturation.initialLoss.coalescedIntermediateCount,
    saturation.increments.coalescedIntermediateCount
  ),
  discardedTerminalCount: saturatingAdd(
    saturation.initialLoss.discardedTerminalCount,
    saturation.increments.discardedTerminalCount
  )
}, saturation.expectedLoss);

console.log('runtime conformance fixtures: PASS');
