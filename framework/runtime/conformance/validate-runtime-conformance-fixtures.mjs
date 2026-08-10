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

const follow = await readFixture('./message-follow-suppression-v1.json');
assert.equal(follow.fixture, 'zlink.framework.message-follow-suppression');
assert.equal(follow.version, 1);
assert.deepEqual(follow.routeFenceFields, [
  'objectKind',
  'logicalObjectId',
  'objectGeneration',
  'targetNodeRid',
  'targetNodeGeneration',
  'authorityOwnerGeneration',
  'ownerLeaseGeneration'
]);
assert.deepEqual(follow.registryInvariants.states, [
  'idle',
  'inFlight',
  'sentUntilExpiry'
]);
assert.equal(follow.registryInvariants.markerCapacityPerRetainedRoute, 1);
assert.equal(follow.registryInvariants.separateSuppressionTimer, false);
assert.equal(follow.registryInvariants.beginReturnsOpaqueClaimToken, true);
assert.equal(follow.registryInvariants.completionRequiresMatchingClaimToken, true);
assert.equal(follow.registryInvariants.staleClaimCannotMutateCurrentMarker, true);

const followKeys = new Set();
for (const [name, key] of Object.entries(follow.keys)) {
  assert.deepEqual(Object.keys(key).sort(), ['sourceRoute', 'targetRoute']);
  for (const route of [key.sourceRoute, key.targetRoute]) {
    assert.deepEqual(Object.keys(route).sort(), follow.routeFenceFields.toSorted(), name);
    for (const field of follow.routeFenceFields) assert.notEqual(route[field], '', `${name}.${field}`);
  }
  assert.equal(key.sourceRoute.objectKind, key.targetRoute.objectKind, name);
  assert.equal(key.sourceRoute.logicalObjectId, key.targetRoute.logicalObjectId, name);
  const serialized = JSON.stringify(key);
  assert.equal(followKeys.has(serialized), false, `${name} duplicates another exact fence`);
  followKeys.add(serialized);
}
uniqueNames(follow.independentKeyMutations, 'name');
const mutatedPaths = new Set();
for (const mutation of follow.independentKeyMutations) {
  const candidate = structuredClone(follow.keys.base);
  for (const path of mutation.paths) {
    const [routeName, field] = path.split('.');
    assert.ok(routeName === 'sourceRoute' || routeName === 'targetRoute', mutation.name);
    assert.ok(follow.routeFenceFields.includes(field), mutation.name);
    assert.notEqual(candidate[routeName][field], mutation.replacement, mutation.name);
    candidate[routeName][field] = mutation.replacement;
    mutatedPaths.add(path);
  }
  assert.notDeepEqual(candidate, follow.keys.base, mutation.name);
}
assert.deepEqual(mutatedPaths, new Set([
  ...follow.routeFenceFields.map((field) => `sourceRoute.${field}`),
  ...follow.routeFenceFields.map((field) => `targetRoute.${field}`)
]));
uniqueNames(follow.scenarios, 'name');
for (const scenario of follow.scenarios) {
  const states = new Map();
  const grantedClaims = new Set();
  for (const operation of scenario.operations) {
    assert.ok(follow.keys[operation.key], `${scenario.name}: unknown key ${operation.key}`);
    const marker = states.get(operation.key);
    const before = marker?.state ?? 'idle';
    let result;
    let after = before;
    let claim = marker?.claim;
    switch (operation.kind) {
      case 'begin':
        if (before === 'idle') {
          assert.equal(typeof operation.claim, 'string', scenario.name);
          assert.notEqual(operation.claim.length, 0, scenario.name);
          assert.equal(grantedClaims.has(operation.claim), false, scenario.name);
          grantedClaims.add(operation.claim);
          result = 'granted';
          after = 'inFlight';
          claim = operation.claim;
        } else {
          result = 'suppressed';
        }
        break;
      case 'markSent':
        assert.equal(before, 'inFlight', scenario.name);
        if (operation.claim !== claim) {
          result = 'ignoredStaleClaim';
        } else {
          result = 'applied';
          after = 'sentUntilExpiry';
        }
        break;
      case 'abort':
        assert.equal(before, 'inFlight', scenario.name);
        if (operation.claim !== claim) {
          result = 'ignoredStaleClaim';
        } else {
          result = 'applied';
          after = 'idle';
          claim = undefined;
        }
        break;
      case 'expireRoute':
        result = 'removed';
        after = 'idle';
        claim = undefined;
        break;
      case 'retainRoute':
        assert.equal(before, 'idle', scenario.name);
        result = 'retained';
        after = 'idle';
        break;
      case 'replaceRoute':
        assert.ok(follow.keys[operation.replacement], scenario.name);
        states.delete(operation.key);
        states.delete(operation.replacement);
        result = 'replaced';
        after = 'idle';
        claim = undefined;
        break;
      default:
        assert.fail(`${scenario.name}: unknown operation ${operation.kind}`);
    }
    if (after === 'idle') states.delete(operation.key);
    else states.set(operation.key, { state: after, claim });
    assert.equal(result, operation.result, scenario.name);
    assert.equal(after, operation.state, scenario.name);
  }
}

const completion = await readFixture('./completion-terminal-v1.json');
assert.equal(completion.fixture, 'zlink.framework.completion-terminal');
assert.equal(completion.version, 1);
assert.deepEqual(completion.limits, {
  pendingOperationCapacity: 4096,
  operationIdBits: 128,
  replyRouteIdBits: 64
});
assert.deepEqual(completion.identityInvariants.operationIdFields, ['high', 'low']);
assert.equal(completion.identityInvariants.operationIdAllZeroAllowed, false);
assert.equal(completion.identityInvariants.replyRouteIdIsOperationId, false);
assert.equal(completion.registryInvariants.terminalWinner, 'atomicTake');
assert.equal(completion.registryInvariants.terminalTombstoneRetention, false);
assert.equal(completion.registryInvariants.completionDispatchedOutsideRegistryGate, true);
assert.equal(
  completion.registryInvariants.sameTerminalDuplicateDiagnostic,
  'duplicateOrLate'
);
assert.equal(
  completion.registryInvariants.conflictingTerminalDiagnostic,
  'protocolOrLate'
);
assert.equal(
  completion.registryInvariants.unknownOperationDiagnostic,
  'unknownOrLate'
);

const operationIds = new Set();
const replyRouteIds = new Set();
for (const operation of Object.values(completion.operations)) {
  const { high, low } = operation.operationId;
  assert.ok(BigInt(high) !== 0n || BigInt(low) !== 0n);
  assert.ok(BigInt(operation.replyRouteId) !== 0n);
  const operationId = `${high}:${low}`;
  assert.equal(operationIds.has(operationId), false);
  assert.equal(replyRouteIds.has(operation.replyRouteId), false);
  operationIds.add(operationId);
  replyRouteIds.add(operation.replyRouteId);
}
uniqueNames(completion.raceScenarios, 'name');
for (const scenario of completion.raceScenarios) {
  assert.ok(completion.operations[scenario.operation]);
  let registered = scenario.registered !== false;
  let applicationTerminal = null;
  const diagnosticEvents = [];
  let completionCount = 0;
  for (const event of scenario.events) {
    if (registered) {
      registered = false;
      applicationTerminal = event;
      completionCount += 1;
    } else {
      diagnosticEvents.push(event);
    }
  }
  assert.equal(applicationTerminal, scenario.applicationTerminal, scenario.name);
  assert.deepEqual(diagnosticEvents, scenario.diagnosticEvents, scenario.name);
  const expectedDiagnosticClass = scenario.name === 'duplicate-reply-does-not-complete-twice'
    ? completion.registryInvariants.sameTerminalDuplicateDiagnostic
    : scenario.name === 'conflicting-late-terminal-is-diagnostic-only'
      ? completion.registryInvariants.conflictingTerminalDiagnostic
      : scenario.name === 'unknown-operation-is-diagnostic-only'
        ? completion.registryInvariants.unknownOperationDiagnostic
        : 'late';
  assert.equal(scenario.diagnosticClass, expectedDiagnosticClass, scenario.name);
  assert.equal(completionCount, scenario.applicationCompletionCount, scenario.name);
}

const ownership = await readFixture('./payload-ownership-v1.json');
assert.equal(ownership.fixture, 'zlink.framework.payload-ownership');
assert.equal(ownership.version, 1);
assert.deepEqual(ownership.ownershipStates, [
  'bindingBorrowed',
  'frameworkOwned',
  'applicationBorrowed',
  'released'
]);
assert.deepEqual(ownership.copyBudget, {
  bindingWithOwnershipHandoff: 0,
  bindingWithoutOwnershipHandoff: 1,
  frameworkCopiesAfterOwnership: 0,
  readonlyAccessorCopies: 0,
  maximumDeserializationsAfterAdmission: 1
});
uniqueNames(ownership.scenarios, 'name');
for (const scenario of ownership.scenarios) {
  assert.ok(['none', 'typed', 'raw'].includes(scenario.handlerKind), scenario.name);
  assert.ok(scenario.deserializations >= 0 && scenario.deserializations <= 1, scenario.name);
  if (scenario.handlerKind === 'typed') {
    assert.equal(scenario.handlerInvocations <= scenario.deserializations, true, scenario.name);
  } else if (scenario.handlerKind === 'raw') {
    assert.equal(scenario.deserializations, 0, scenario.name);
  }
  if (!scenario.admitted) {
    assert.equal(scenario.frameworkOwnershipAcquired, false, scenario.name);
    assert.equal(scenario.deserializations, 0, scenario.name);
    assert.equal(scenario.frameworkReleases, 0, scenario.name);
  } else {
    assert.equal(scenario.frameworkOwnershipAcquired, true, scenario.name);
    assert.equal(scenario.frameworkReleases, 1, scenario.name);
  }
}
assert.equal(ownership.accessorScenario.fullBufferCopies, 0);
assert.equal(ownership.accessorScenario.viewIdentityIsNotContractual, true);
assert.equal(ownership.accessorScenario.backingOwner, 'framework');

const codec = await readFixture('./codec-selection-v1.json');
assert.equal(codec.fixture, 'zlink.framework.codec-selection');
assert.equal(codec.version, 1);
assert.equal(codec.limits.sendTypeCacheCapacity, 1024);
assert.deepEqual(codec.contentTypeNormalization, {
  registrationInput: 'bareMediaType',
  outerWhitespace: 'trimAsciiSpaceAndTab',
  typeAndSubtypeCase: 'asciiLowercase',
  tokenCharacters: "!#$%&'*+-.^_`|~0-9A-Za-z",
  parametersAllowed: false,
  internalWhitespaceAllowed: false,
  wireRequiresCanonicalForm: true,
  nonCanonicalWireTerminal: 'protocolError'
});
assert.equal(codec.registryInvariants.immutableAfterStartup, true);
assert.equal(
  codec.registryInvariants.sameNormalizedContentTypeRegistration,
  'lastRegistrationReplaces'
);
assert.equal(codec.registryInvariants.sendSelectorInput, 'declaredMessageType');
assert.equal(codec.registryInvariants.sendSelectorPriority, 'lastRegisteredMatch');
assert.equal(codec.registryInvariants.unknownReceiveContentType, 'protocolError');
assert.equal(codec.registryInvariants.unknownReceiveFallsBackToJson, false);

uniqueNames(codec.extensions, 'name');
uniqueNames(codec.extensions, 'contentType');
const extensions = Object.fromEntries(codec.extensions.map((entry) => [entry.name, entry]));
const mediaTypeToken = /^[!#$%&'*+\-.^_`|~0-9A-Za-z]+$/;
const normalizeRegistrationContentType = (input) => {
  const trimmed = input.replace(/^[\t ]+|[\t ]+$/g, '');
  const separator = trimmed.indexOf('/');
  if (separator <= 0 || separator !== trimmed.lastIndexOf('/')) return null;
  const type = trimmed.slice(0, separator);
  const subtype = trimmed.slice(separator + 1);
  if (!mediaTypeToken.test(type) || !mediaTypeToken.test(subtype)) return null;
  return `${type.toLowerCase()}/${subtype.toLowerCase()}`;
};
uniqueNames(codec.normalizationScenarios, 'name');
for (const scenario of codec.normalizationScenarios) {
  const normalized = normalizeRegistrationContentType(scenario.input);
  if (scenario.expectedError === undefined) {
    assert.equal(normalized, scenario.expected, scenario.name);
  } else {
    assert.equal(normalized, null, scenario.name);
    assert.equal(scenario.expectedError, 'configurationError', scenario.name);
  }
}
const normalizedDuplicateTable = new Map();
for (const [index, input] of codec.normalizedDuplicateScenario.registrationInputs.entries()) {
  const normalized = normalizeRegistrationContentType(input);
  assert.notEqual(normalized, null);
  normalizedDuplicateTable.set(normalized, index);
}
assert.equal(
  normalizedDuplicateTable.size,
  codec.normalizedDuplicateScenario.finalEntryCount
);
assert.deepEqual(
  [...normalizedDuplicateTable.values()],
  [codec.normalizedDuplicateScenario.selectedRegistrationIndex]
);
for (const extension of codec.extensions) {
  assert.equal(
    extension.contentType,
    normalizeRegistrationContentType(extension.contentType),
    extension.name
  );
}
uniqueNames(codec.sendScenarios, 'name');
for (const scenario of codec.sendScenarios) {
  let selected;
  for (const name of scenario.registrationOrder) {
    const extension = extensions[name];
    assert.ok(extension, `${scenario.name}: unknown extension ${name}`);
    if (extension.matchesDeclaredTypes.includes(scenario.declaredType)) selected = extension;
  }
  assert.equal(selected?.name ?? 'json', scenario.expectedCodec, scenario.name);
  assert.equal(selected?.contentType ?? 'application/json', scenario.expectedContentType, scenario.name);
}
uniqueNames(codec.receiveScenarios, 'name');
const receiveTable = new Map(codec.extensions.map((entry) => [entry.contentType, entry.name]));
for (const scenario of codec.receiveScenarios) {
  const selected = receiveTable.get(scenario.wireContentType) ?? null;
  assert.equal(selected, scenario.expectedCodec, scenario.name);
  assert.equal(selected === null ? 'protocolError' : 'success', scenario.expectedTerminal, scenario.name);
}
for (const scenario of codec.cacheScenarios) {
  assert.equal(
    scenario.cachedTypes,
    Math.min(scenario.distinctDeclaredTypes, codec.limits.sendTypeCacheCapacity),
    scenario.name
  );
  assert.equal(
    scenario.uncachedTypes,
    scenario.distinctDeclaredTypes - scenario.cachedTypes,
    scenario.name
  );
  assert.equal(scenario.existingCachedSelectionChanges, false, scenario.name);
}

const runtimeState = await readFixture('./runtime-state-v1.json');
assert.equal(runtimeState.fixture, 'zlink.framework.runtime-state');
assert.equal(runtimeState.version, 1);
assert.deepEqual(runtimeState.publicStates, [
  { name: 'preparing', wireValue: 0, isReady: false },
  { name: 'serving', wireValue: 1, isReady: true },
  { name: 'relocating', wireValue: 2, isReady: false },
  { name: 'relocated', wireValue: 3, isReady: false },
  { name: 'draining', wireValue: 4, isReady: false },
  { name: 'stopped', wireValue: 5, isReady: false },
  { name: 'error', wireValue: 6, isReady: false }
]);
assert.equal(runtimeState.authorityInvariants.publicStateIsSingleReadinessAuthority, true);
assert.equal(runtimeState.authorityInvariants.independentMutableReadinessAuthority, false);
assert.equal(runtimeState.authorityInvariants.publicIsReadyDerivedOnlyFromState, true);
assert.equal(
  runtimeState.authorityInvariants.acceptingWorkFormula,
  'stateIsServingAndAdmissionOpen'
);
uniqueNames(runtimeState.startupPrerequisites.map((name) => ({ name })), 'name');
const publicStateNames = new Set(runtimeState.publicStates.map(({ name }) => name));
for (const scenario of runtimeState.acceptingWorkScenarios) {
  assert.ok(publicStateNames.has(scenario.state));
  assert.equal(
    scenario.state === 'serving' && scenario.admissionOpen,
    scenario.expected
  );
}
assert.deepEqual(runtimeState.maintenanceAdmissionProjection, {
  preparing: 'preparing',
  serving: 'serving',
  relocating: 'retiring',
  relocated: 'retiring',
  draining: 'draining',
  stopped: 'stopped',
  error: 'error'
});
assert.deepEqual(runtimeState.discoveryAvailabilityProjection, {
  preparing: 'preparing',
  serving: 'serving',
  relocating: 'retiring',
  relocated: 'retiring',
  draining: 'retiring',
  stopped: 'stopped',
  error: 'error',
  transportOnlyState: 'disconnected'
});
for (const scenario of runtimeState.topologyScenarios) {
  assert.ok(publicStateNames.has(scenario.hostState));
  assert.equal(
    scenario.hostState === 'serving' && scenario.readyTargetCount > 0,
    scenario.isReady
  );
}

console.log('runtime conformance fixtures: PASS');
