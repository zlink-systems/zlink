#!/usr/bin/env node

import assert from 'node:assert/strict';
import { access, readFile } from 'node:fs/promises';
import { resolve } from 'node:path';
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

const relocation = await readFixture('./relocation-behavior-v1.json');
const boundSessionRelocation = await readFixture('./bound-session-relocation-v1.json');
const relocationAdapters = await readFixture('./relocation-conformance-adapters-v1.json');
const relocationE2e = await readFixture('./relocation-e2e-scenarios-v1.json');
const repositoryRoot = fileURLToPath(new URL('../../../', import.meta.url));
const requiredRelocationLanguages = ['cpp', 'dotnet', 'java', 'kotlin', 'node'];
const requiredRelocationGroups = [
  'commonLifecycle',
  'actorJoin',
  'actorJoinMilestones',
  'actorHostHandoff',
  'actorBoundSession',
  'actorMessageFollow',
  'spotWholeSpot',
  'spotPerActor',
  'instanceSpot',
  'statePolicies',
  'failureAndIdempotency'
];

assert.equal(relocation.fixture, 'zlink.framework.relocation-behavior');
assert.equal(relocation.version, 1);
assert.deepEqual(relocation.contractSources, [
  {
    path: 'framework/doc/framework/common/spec/server/15-spot-actor.ko.md',
    sections: ['4.2', '5', '6', '7', '8']
  },
  {
    path: 'framework/doc/framework/common/spec/server/30-host-relocation-flow.ko.md',
    sections: ['7', '8', '14']
  },
  {
    path: 'framework/doc/framework/common/spec/server/20-session-actor-dispatch.ko.md',
    sections: ['5', '6', '7']
  },
  {
    path: 'framework/doc/framework/common/spec/server/21-location-runtime.ko.md',
    sections: ['7']
  }
]);
for (const source of relocation.contractSources)
  await access(resolve(repositoryRoot, source.path));
assert.deepEqual(relocation.modelBoundary.excludedImplementationDetails, [
  'wireCommandNumber',
  'functionName',
  'lockType',
  'threadOrEventLoopChoice'
]);
assert.equal(relocation.modelBoundary.partialOrderOnly, true);
assert.equal(Object.hasOwn(relocation.commonLifecycle, 'commands'), false);
assert.doesNotMatch(JSON.stringify(relocation), /command(?:42|43|44|45)/i);

uniqueNames(relocation.eventVocabulary.map((name) => ({ name })), 'name');
const relocationEvents = new Set(relocation.eventVocabulary);
const assertEdges = (edges, owner) => {
  const keys = new Set();
  for (const edge of edges) {
    assert.equal(edge.length, 2, `${owner}: edge length`);
    const [before, after] = edge;
    assert.ok(relocationEvents.has(before), `${owner}: unknown event ${before}`);
    assert.ok(relocationEvents.has(after), `${owner}: unknown event ${after}`);
    assert.notEqual(before, after, `${owner}: self edge`);
    assert.ok(!keys.has(`${before}\0${after}`), `${owner}: duplicate edge`);
    keys.add(`${before}\0${after}`);
  }
};
assert.deepEqual(relocation.commonLifecycle.requiredEvents, [
  'relocationRequested',
  'sourceAdmissionSealed',
  'sourceStateCaptured',
  'targetTemporaryQueueOpened',
  'targetStateRestored',
  'ownershipCommitted',
  'targetLifecycleCompleted',
  'savedWorkAdmitted',
  'temporaryWorkAdmitted',
  'targetDispatchActivated',
  'targetReadyPublished',
  'sourceCleanupStarted',
  'sourceCleanupCompleted',
  'authorityCompleted'
]);
assert.deepEqual(relocation.commonLifecycle.requiredOrder, [
  ['relocationRequested', 'sourceAdmissionSealed'],
  ['sourceAdmissionSealed', 'sourceStateCaptured'],
  ['sourceStateCaptured', 'targetTemporaryQueueOpened'],
  ['targetTemporaryQueueOpened', 'targetStateRestored'],
  ['targetStateRestored', 'ownershipCommitted'],
  ['ownershipCommitted', 'targetLifecycleCompleted'],
  ['targetLifecycleCompleted', 'savedWorkAdmitted'],
  ['savedWorkAdmitted', 'temporaryWorkAdmitted'],
  ['temporaryWorkAdmitted', 'targetDispatchActivated'],
  ['targetDispatchActivated', 'targetReadyPublished'],
  ['targetReadyPublished', 'sourceCleanupStarted'],
  ['sourceCleanupStarted', 'sourceCleanupCompleted'],
  ['sourceCleanupCompleted', 'authorityCompleted']
]);
assertEdges(relocation.commonLifecycle.requiredOrder, 'commonLifecycle');

const allRelocationEdges = [...relocation.commonLifecycle.requiredOrder];
for (const branch of relocation.optionalBranches) {
  assertEdges(branch.requiredOrder, branch.name);
  allRelocationEdges.push(...branch.requiredOrder);
}
for (const profile of relocation.profiles)
  allRelocationEdges.push(...profile.additionalOrder);
for (const journey of relocation.journeyScenarios) {
  assertEdges(journey.requiredOrder, journey.name);
  allRelocationEdges.push(...journey.requiredOrder);
}
for (const scenario of relocation.trafficScenarios) {
  if (!scenario.requiredOrder) continue;
  assertEdges(scenario.requiredOrder, scenario.name);
}
const assertRelocationAcyclic = (edges, owner) => {
  const successors = new Map(relocation.eventVocabulary.map(name => [name, []]));
  const indegree = new Map(relocation.eventVocabulary.map(name => [name, 0]));
  for (const [before, after] of edges) {
    successors.get(before).push(after);
    indegree.set(after, indegree.get(after) + 1);
  }
  const ready = [...indegree].filter(([, count]) => count === 0).map(([name]) => name);
  let visited = 0;
  while (ready.length > 0) {
    const current = ready.pop();
    visited += 1;
    for (const next of successors.get(current)) {
      indegree.set(next, indegree.get(next) - 1);
      if (indegree.get(next) === 0) ready.push(next);
    }
  }
  assert.equal(visited, relocation.eventVocabulary.length, `${owner}: partial order has a cycle`);
};
assertRelocationAcyclic(allRelocationEdges, 'relocation');
for (const scenario of relocation.trafficScenarios) {
  if (!scenario.requiredOrder) continue;
  assertRelocationAcyclic(
    [...allRelocationEdges, ...scenario.requiredOrder],
    scenario.name
  );
}

assert.deepEqual(relocation.commonLifecycle.readyPredecessors, [
  'targetLifecycleCompleted',
  'savedWorkAdmitted',
  'temporaryWorkAdmitted',
  'targetDispatchActivated'
]);
assert.equal(relocation.commonLifecycle.cleanupPredecessor, 'targetReadyPublished');
assert.deepEqual(relocation.commonLifecycle.admissionCheckpoints, [
  { name: 'beforeOwnershipCommit', currentOwner: 'source', admittingOwners: ['source'] },
  { name: 'afterOwnershipCommitBeforeReady', currentOwner: 'target', admittingOwners: [] },
  { name: 'afterTargetReady', currentOwner: 'target', admittingOwners: ['target'] }
]);
assert.deepEqual(relocation.commonLifecycle.independence, {
  publicJoinCompletionWaitsForSessionRouteConvergence: false,
  targetReadyWaitsForSessionRouteConvergence: false,
  targetMessageProcessingWaitsForSessionRouteConvergence: false,
  sourceCleanupStartsBeforeTargetReady: false
});
assert.deepEqual(relocation.commonLifecycle.identityAndAuthority, {
  objectGenerationStable: true,
  targetOwnerGenerationRelation: 'strictlyGreaterThanSource',
  onlyCurrentOwnerAdmits: true,
  originalOperationIdPreserved: true,
  originalReplyRoutePreserved: true
});
assert.deepEqual(relocation.optionalBranches, [
  {
    name: 'boundSessionRoute',
    profiles: [
      'actorJoin',
      'actorHostHandoff',
      'perActorUserSpot',
      'spotWideUserSpot'
    ],
    events: [
      'sessionRouteConverged',
      'sessionRouteTerminalDelivered'
    ],
    requiredOrder: [
      ['authorityCompleted', 'sessionRouteConverged'],
      ['sessionRouteConverged', 'sessionRouteTerminalDelivered']
    ]
  }
]);

uniqueNames(relocation.profiles, 'name');
assert.deepEqual(relocation.profiles.map(({ name }) => name), [
  'actorJoin',
  'actorHostHandoff',
  'perActorUserSpot',
  'spotWideUserSpot',
  'instanceSpot'
]);
for (const profile of relocation.profiles) {
  assertEdges(profile.additionalOrder, profile.name);
  const categories = [profile.requiredEvents, profile.optionalEvents, profile.forbiddenEvents];
  for (const category of categories)
    for (const event of category)
      assert.ok(relocationEvents.has(event), `${profile.name}: unknown event ${event}`);
  assert.equal(
    new Set(categories.flat()).size,
    categories.flat().length,
    `${profile.name}: event categories overlap`
  );
}
const profiles = Object.fromEntries(relocation.profiles.map(profile => [profile.name, profile]));
assert.deepEqual(profiles.actorJoin.requiredEvents, [
  'sourceMembershipLeaveSubmitted',
  'publicJoinCompleted'
]);
assert.deepEqual(profiles.actorJoin.additionalOrder, [
  ['targetLifecycleCompleted', 'sourceMembershipLeaveSubmitted'],
  ['sourceMembershipLeaveSubmitted', 'publicJoinCompleted'],
  ['publicJoinCompleted', 'savedWorkAdmitted']
]);
assert.equal(profiles.actorJoin.terminalEvent, 'publicJoinCompleted');
assert.deepEqual(profiles.actorJoin.independence, {
  publicJoinCompletionWaitsForSourceMembershipLeaveResult: false
});
assert.ok(profiles.actorJoin.forbiddenEvents.includes('relocationTerminalDelivered'));
assert.deepEqual(profiles.actorHostHandoff.requiredEvents, [
  'relocationTerminalDelivered'
]);
assert.deepEqual(profiles.actorHostHandoff.additionalOrder, [
  ['authorityCompleted', 'relocationTerminalDelivered']
]);
assert.equal(profiles.actorHostHandoff.terminalEvent, 'relocationTerminalDelivered');
assert.equal(profiles.actorHostHandoff.callbackCounts.targetMembershipJoin, 0);
assert.equal(profiles.actorHostHandoff.callbackCounts.sourceMembershipLeave, 0);
assert.equal(profiles.spotWideUserSpot.callbackCounts.targetMembershipJoin, 0);
assert.equal(profiles.spotWideUserSpot.callbackCounts.sourceMembershipLeave, 0);
assert.ok(profiles.instanceSpot.forbiddenEvents.includes('sessionRouteConverged'));
for (const profileName of ['perActorUserSpot', 'spotWideUserSpot', 'instanceSpot']) {
  assert.deepEqual(profiles[profileName].requiredEvents, [
    'sourceClosingCompleted',
    'relocationTerminalDelivered'
  ]);
  assert.deepEqual(profiles[profileName].additionalOrder, [
    ['targetReadyPublished', 'sourceClosingCompleted'],
    ['sourceCleanupStarted', 'sourceClosingCompleted'],
    ['sourceClosingCompleted', 'sourceCleanupCompleted'],
    ['authorityCompleted', 'relocationTerminalDelivered']
  ]);
  assert.equal(profiles[profileName].sourceClosingReason, 'RelocationOut');
  assert.equal(profiles[profileName].terminalEvent, 'relocationTerminalDelivered');
}

uniqueNames(relocation.statePolicies, 'name');
assert.deepEqual(relocation.statePolicies, [
  {
    name: 'DisableRelocation',
    relocationAccepted: false,
    sourceAuthorityRetained: true,
    sourceAdmissionRetained: true,
    targetFactoryInvocationCount: 0,
    applicationStateCaptureCount: 0,
    applicationStateRestoreCount: 0,
    targetApplicationHandlerCount: 0,
    frameworkPendingWorkPreserved: true,
    frameworkPendingWorkTransferred: false
  },
  {
    name: 'RecreateOnRelocation',
    relocationAccepted: true,
    sourceAuthorityRetained: false,
    sourceAdmissionRetained: false,
    targetFactoryInvocationCount: 1,
    applicationStateCaptureCount: 0,
    applicationStateRestoreCount: 0,
    frameworkPendingWorkPreserved: true,
    frameworkPendingWorkTransferred: true
  },
  {
    name: 'PreserveStateWith',
    relocationAccepted: true,
    sourceAuthorityRetained: false,
    sourceAdmissionRetained: false,
    targetFactoryInvocationCount: 1,
    applicationStateCaptureCount: 1,
    applicationStateRestoreCount: 1,
    frameworkPendingWorkPreserved: true,
    frameworkPendingWorkTransferred: true
  }
]);

uniqueNames(relocation.trafficScenarios, 'name');
const relocationTraffic = Object.fromEntries(
  relocation.trafficScenarios.map(scenario => [scenario.name, scenario])
);
const inFlight = relocationTraffic['accepted-in-flight-request'];
assert.deepEqual(inFlight.deliveredOperationIds, [inFlight.originalOperationId]);
assert.deepEqual(inFlight.repliedOperationIds, [inFlight.originalOperationId]);
assert.deepEqual(inFlight.observedReplyRouteIds, [inFlight.originalReplyRouteId]);
assert.equal(inFlight.deliveryCount, 1);
assert.equal(inFlight.replyCount, 1);
assert.equal(inFlight.settlementCount, 1);
assert.equal(inFlight.lossCount, 0);
assert.equal(inFlight.duplicateCount, 0);
assert.equal(inFlight.sameCallResubmitCount, 0);

const callbackPush = relocationTraffic['target-lifecycle-callback-bound-session-push'];
assert.equal(callbackPush.profile, 'actorJoin');
assert.equal(callbackPush.origin, 'targetLifecycleCallbackWhileBoundSessionSealed');
assertEdges(callbackPush.requiredOrder, callbackPush.name);
for (const requiredEdge of [
  ['ownershipCommitted', 'applicationOperationSubmitted'],
  ['sourceAdmissionSealed', 'applicationOperationSubmitted'],
  ['applicationOperationSubmitted', 'targetLifecycleCompleted'],
  ['targetLifecycleCompleted', 'sourceMembershipLeaveSubmitted'],
  ['sourceMembershipLeaveSubmitted', 'publicJoinCompleted'],
  ['applicationOperationSubmitted', 'sessionRouteConverged'],
  ['sessionRouteConverged', 'applicationOperationDelivered']
])
  assert.ok(
    callbackPush.requiredOrder.some(edge => edge[0] === requiredEdge[0]
      && edge[1] === requiredEdge[1]),
    `${callbackPush.name}: missing ${requiredEdge.join(' -> ')}`
  );
assert.deepEqual(callbackPush.checkpoints, [
  { name: 'afterCallbackSubmit', deliveryCount: 0, settlementCount: 0, payloadReleaseCount: 0 },
  { name: 'beforeSessionRouteConverged', deliveryCount: 0, settlementCount: 0, payloadReleaseCount: 0 },
  { name: 'afterSessionRouteConverged', deliveryCount: 1, settlementCount: 1, payloadReleaseCount: 1 },
  { name: 'afterDuplicateRouteTerminal', deliveryCount: 1, settlementCount: 1, payloadReleaseCount: 1 }
]);
assert.equal(callbackPush.lossCount, 0);
assert.equal(callbackPush.duplicateCount, 0);
assert.equal(callbackPush.sameCallResubmitCount, 0);

const genericTargetPush =
  relocationTraffic['target-bound-session-push-before-route-convergence'];
assert.deepEqual(genericTargetPush.profiles, [
  'actorHostHandoff',
  'perActorUserSpot',
  'spotWideUserSpot'
]);
assert.equal(genericTargetPush.origin, 'targetApplicationWhileBoundSessionRoutePending');
assertEdges(genericTargetPush.requiredOrder, genericTargetPush.name);
assert.deepEqual(genericTargetPush.requiredOrder, [
  ['targetReadyPublished', 'applicationOperationSubmitted'],
  ['applicationOperationSubmitted', 'sessionRouteConverged'],
  ['sessionRouteConverged', 'applicationOperationDelivered'],
  ['applicationOperationDelivered', 'applicationOperationSettled']
]);
assert.equal(genericTargetPush.lossCount, 0);
assert.equal(genericTargetPush.duplicateCount, 0);
assert.equal(genericTargetPush.sameCallResubmitCount, 0);

const messageFollow = relocationTraffic['message-follow-request'];
assert.equal(messageFollow.followOperationId, messageFollow.originalOperationId);
assert.equal(messageFollow.followReplyRouteId, messageFollow.originalReplyRouteId);
assert.equal(messageFollow.deliveryCount, 1);
assert.equal(messageFollow.replyCount, 1);
assert.equal(messageFollow.settlementCount, 1);
assert.equal(messageFollow.lossCount, 0);
assert.equal(messageFollow.duplicateCount, 0);
assert.equal(messageFollow.sameCallResubmitCount, 0);

const delayedMessageFollow = relocationTraffic['message-follow-delayed-notice-next-call'];
assert.equal(delayedMessageFollow.origin, 'positiveSourceRouteCacheWithDelayedRelayNotice');
assertEdges(delayedMessageFollow.requiredOrder, delayedMessageFollow.name);
assert.deepEqual(delayedMessageFollow.requiredOrder, [
  ['messageFollowRelayDelivered', 'messageFollowReplyCompleted'],
  ['messageFollowReplyCompleted', 'sessionRouteConverged'],
  ['sessionRouteConverged', 'nextApplicationOperationSubmitted'],
  ['nextApplicationOperationSubmitted', 'nextApplicationOperationDelivered'],
  ['nextApplicationOperationDelivered', 'relayNoticeObserved'],
  ['relayNoticeObserved', 'followingApplicationOperationSubmitted'],
  ['followingApplicationOperationSubmitted', 'followingApplicationOperationDelivered'],
  ['followingApplicationOperationDelivered', 'messageFollowExpired']
]);
assert.deepEqual(delayedMessageFollow.checkpoints, [
  {
    name: 'afterFirstReplyBeforeNotice',
    deliveryCount: 1,
    replyCount: 1,
    settlementCount: 1,
    sourceHandlerCount: 0,
    targetHandlerCount: 1
  },
  {
    name: 'afterSessionRouteConvergedBeforeNotice',
    deliveryCount: 1,
    replyCount: 1,
    settlementCount: 1,
    sourceHandlerCount: 0,
    targetHandlerCount: 1
  },
  {
    name: 'afterImmediateNextOperation',
    deliveryCount: 2,
    replyCount: 1,
    settlementCount: 2,
    sourceHandlerCount: 0,
    targetHandlerCount: 2
  },
  {
    name: 'afterRelayNotice',
    deliveryCount: 2,
    replyCount: 1,
    settlementCount: 2,
    sourceHandlerCount: 0,
    targetHandlerCount: 2
  },
  {
    name: 'afterFollowingOperation',
    deliveryCount: 3,
    replyCount: 1,
    settlementCount: 3,
    sourceHandlerCount: 0,
    targetHandlerCount: 3
  },
  {
    name: 'afterMessageFollowExpiry',
    deliveryCount: 3,
    replyCount: 1,
    settlementCount: 3,
    sourceHandlerCount: 0,
    targetHandlerCount: 3
  }
]);
assert.equal(delayedMessageFollow.lossCount, 0);
assert.equal(delayedMessageFollow.duplicateCount, 0);
assert.equal(delayedMessageFollow.sameCallResubmitCount, 0);

uniqueNames(relocation.journeyScenarios, 'name');
const roundTrip = relocation.journeyScenarios.find(
  ({ name }) => name === 'bound-actor-round-trip-with-delayed-route-notice'
);
assert.ok(roundTrip);
assert.equal(roundTrip.profile, 'actorJoin');
assert.deepEqual(roundTrip.owners, {
  initial: 'ownerA',
  firstTarget: 'ownerB',
  returnTarget: 'ownerA'
});
assert.deepEqual(roundTrip.identity, {
  actorIdStable: true,
  objectGenerationStable: true,
  sessionIdentityStable: true,
  ownerTenuresDistinct: true
});
assert.deepEqual(roundTrip.routeCacheTenures, {
  initialSource: 'ownerA#0',
  firstTarget: 'ownerB#1',
  returnTarget: 'ownerA#2'
});
assert.equal(roundTrip.lateNoticePolicy, 'compareAndRemoveExactSourceTenureOnly');
assert.deepEqual(roundTrip.requiredOrder, [
  ['firstRelocationStable', 'followedRequestReplied'],
  ['followedRequestReplied', 'returnOperationSubmitted'],
  ['returnOperationSubmitted', 'returnOperationDeliveredAtCurrentOwner'],
  ['returnOperationDeliveredAtCurrentOwner', 'successorRelocationRequested'],
  ['successorRelocationRequested', 'successorRelocationAdmitted'],
  ['successorRelocationAdmitted', 'secondRelocationStable'],
  ['secondRelocationStable', 'roundTripSessionNotificationDelivered'],
  ['roundTripSessionNotificationDelivered', 'latePredecessorNoticeObserved'],
  ['latePredecessorNoticeObserved', 'postRoundTripRequestReplied']
]);
assert.deepEqual(roundTrip.successorPolicy, {
  maxActiveRelocationBarriers: 1,
  startsBeforePredecessorRouteTerminal: false,
  eventuallyAdmittedAfterPredecessorRouteTerminal: true,
  parallelTemporaryQueueCount: 0
});
assert.deepEqual(roundTrip.checkpoints, [
  {
    name: 'afterFirstRelocationStable',
    currentOwner: 'ownerB',
    activeRelocationBarrierCount: 0,
    boundSessionRouteOwner: 'ownerB',
    routeCacheTenure: 'ownerA#0'
  },
  {
    name: 'afterFollowedRequestBeforeNotice',
    sourceHandlerCount: 0,
    currentOwnerHandlerCount: 1,
    replyCount: 1
  },
  {
    name: 'afterReturnOperationDelivered',
    returnOperationDeliveryCount: 1,
    returnOperationOwner: 'ownerB',
    sourceHandlerCount: 0,
    lossCount: 0,
    duplicateCount: 0
  },
  {
    name: 'afterSecondRelocationStable',
    currentOwner: 'ownerA',
    activeRelocationBarrierCount: 0,
    boundSessionRouteOwner: 'ownerA',
    routeCacheTenure: 'ownerA#2',
    actorIdStable: true,
    objectGenerationStable: true,
    sessionIdentityStable: true
  },
  {
    name: 'afterLatePredecessorNotice',
    currentOwner: 'ownerA',
    boundSessionRouteOwner: 'ownerA',
    noticeSourceTenure: 'ownerA#0',
    routeCacheTenureBeforeNotice: 'ownerA#2',
    routeCacheTenureAfterNotice: 'ownerA#2',
    exactSourceTenureMatched: false,
    additionalCacheMutationCount: 0,
    additionalRouteMutationCount: 0,
    additionalDeliveryCount: 0
  },
  {
    name: 'afterPostRoundTripRequest',
    requestHandlerOwner: 'ownerA',
    requestHandlerCount: 1,
    replyCount: 1,
    lossCount: 0,
    duplicateCount: 0
  }
]);

uniqueNames(relocation.routeCacheCompareAndRemoveScenarios, 'name');
assert.deepEqual(
  relocation.routeCacheCompareAndRemoveScenarios.map(({ name }) => name),
  [
    'exact-source-tenure-removes-matching-cache-once',
    'aba-source-tenure-does-not-remove-current-cache'
  ]
);
for (const scenario of relocation.routeCacheCompareAndRemoveScenarios) {
  let cacheTenure = scenario.cachedTenure;
  let removalCount = 0;
  let preserveCount = 0;
  if (cacheTenure === scenario.noticeSourceTenure) {
    cacheTenure = null;
    removalCount += 1;
  } else {
    preserveCount += 1;
  }
  assert.equal(cacheTenure, scenario.expectedCacheTenure, scenario.name);
  assert.equal(removalCount, scenario.removalCount, scenario.name);
  assert.equal(preserveCount, scenario.preserveCount, scenario.name);
}

uniqueNames(relocation.idempotencyScenarios, 'name');
const duplicateTargetAttempt = relocation.idempotencyScenarios.find(
  ({ name }) => name === 'duplicate-target-attempt-reuses-existing-progress'
);
assert.ok(duplicateTargetAttempt);
assert.deepEqual(duplicateTargetAttempt.profiles, [
  'actorJoin',
  'actorHostHandoff',
  'perActorUserSpot',
  'spotWideUserSpot',
  'instanceSpot'
]);
assert.deepEqual(duplicateTargetAttempt.duplicateKeyFields, [
  'relocationId',
  'targetAttempt'
]);
assert.equal(duplicateTargetAttempt.duplicateRequestUsesSameKey, true);
assert.deepEqual(duplicateTargetAttempt.duplicateFingerprintFields, [
  'sourceTenure',
  'targetTenure',
  'statePolicy',
  'capturedPayloadDigest'
]);
assert.equal(duplicateTargetAttempt.duplicateRequestUsesIdenticalFingerprint, true);
const targetAttemptCounts = (applicationStateRestoreCount) => ({
  targetTemporaryQueueOpenedCount: 1,
  targetFactoryInvocationCount: 1,
  applicationStateRestoreCount
});
assert.deepEqual(duplicateTargetAttempt.statePolicyExpectations, {
  RecreateOnRelocation: {
    afterInitialRequest: targetAttemptCounts(0),
    afterDuplicateRequest: targetAttemptCounts(0)
  },
  PreserveStateWith: {
    afterInitialRequest: targetAttemptCounts(1),
    afterDuplicateRequest: targetAttemptCounts(1)
  }
});
assert.deepEqual(duplicateTargetAttempt.duplicateAdditionalObservableCounts, {
  targetTemporaryQueueOpened: 0,
  targetFactoryInvocation: 0,
  applicationStateRestore: 0,
  targetLifecycleCallback: 0,
  sourceMembershipLeaveSubmission: 0,
  publicJoinCompletionCallback: 0,
  sourceClosingCallback: 0,
  relocationTerminal: 0
});
const conflictingTargetAttempt = relocation.idempotencyScenarios.find(
  ({ name }) => name === 'same-target-attempt-key-with-different-fingerprint-is-rejected'
);
assert.ok(conflictingTargetAttempt);
assert.equal(conflictingTargetAttempt.sameKey, true);
assert.equal(conflictingTargetAttempt.sameFingerprint, false);
assert.equal(conflictingTargetAttempt.result, 'conflict');
for (const countField of [
  'additionalTargetFactoryInvocationCount',
  'additionalStateRestoreCount',
  'additionalLifecycleCallbackCount',
  'additionalAuthorityMutationCount',
  'additionalTerminalDeliveryCount'
])
  assert.equal(conflictingTargetAttempt[countField], 0, countField);

const successorFence = relocation.idempotencyScenarios.find(
  ({ name }) => name === 'late-terminal-does-not-cross-successor-relocation-fence'
);
assert.ok(successorFence);
assert.deepEqual(successorFence.profiles, [
  'actorJoin',
  'actorHostHandoff',
  'perActorUserSpot',
  'spotWideUserSpot'
]);
assert.equal(successorFence.sameObjectGeneration, true);
assert.deepEqual(successorFence.successorFenceFields, [
  'relocationId',
  'bindingGeneration',
  'sessionIdentity'
]);
assert.deepEqual(successorFence.checkpoints, [
  {
    name: 'afterSuccessorSubmitBeforeOldTerminal',
    deliveryCount: 0,
    settlementCount: 0,
    payloadReleaseCount: 0,
    successorAdmissionCount: 0
  },
  {
    name: 'afterOldTerminal',
    deliveryCount: 0,
    settlementCount: 0,
    payloadReleaseCount: 0
  },
  {
    name: 'afterSuccessorTerminal',
    deliveryCount: 1,
    settlementCount: 1,
    payloadReleaseCount: 1,
    successorAdmissionCount: 1
  },
  {
    name: 'afterDuplicateOldAndSuccessorTerminals',
    deliveryCount: 1,
    settlementCount: 1,
    payloadReleaseCount: 1
  }
]);
assert.equal(successorFence.oldTerminalAdditionalDeliveryCount, 0);
assert.equal(successorFence.oldTerminalAdditionalSettlementCount, 0);
assert.equal(successorFence.oldTerminalAdditionalPayloadReleaseCount, 0);

uniqueNames(relocation.terminalScenarios, 'name');
const relocationTerminals = Object.fromEntries(
  relocation.terminalScenarios.map(scenario => [scenario.name, scenario])
);
assert.deepEqual(Object.keys(relocationTerminals), [
  'duplicate-success-terminal',
  'precommit-abort',
  'postcommit-target-failure',
  'stale-owner',
  'aba-generation',
  'session-or-binding-closed',
  'operation-timeout',
  'runtime-shutdown'
]);
for (const scenario of relocation.terminalScenarios) {
  assert.equal(scenario.settlementCount, 1, `${scenario.name}: settlement`);
  assert.equal(scenario.cleanupCount, 1, `${scenario.name}: cleanup`);
  assert.equal(scenario.lateTerminalAdditionalDeliveryCount, 0, scenario.name);
  assert.equal(scenario.lateTerminalAdditionalSettlementCount, 0, scenario.name);
  assert.equal(scenario.lateTerminalAdditionalCleanupCount, 0, scenario.name);
  assert.equal(scenario.lateTerminalAdditionalRouteMutationCount, 0, scenario.name);
}
assert.equal(relocationTerminals['duplicate-success-terminal'].routeMutationCount, 1);
assert.equal(relocationTerminals['duplicate-success-terminal'].deliveryCount, 1);
assert.equal(relocationTerminals['precommit-abort'].targetApplicationExecutionCount, 0);
assert.equal(relocationTerminals['precommit-abort'].sourceAuthorityRetained, true);
assert.equal(relocationTerminals['precommit-abort'].sourceHeldTrafficRestored, true);
assert.equal(relocationTerminals['precommit-abort'].restoredSourceTrafficDeliveryCount, 1);
assert.equal(relocationTerminals['postcommit-target-failure'].sourceRollbackCount, 0);
assert.equal(relocationTerminals['postcommit-target-failure'].sourceAdmissionResumeCount, 0);

assert.deepEqual(relocation.actorCapabilities.boundSessionBarrier, {
  postSealTrafficRetained: true,
  acceptedHighWaterComesFromSessionOwner: true,
  routeConvergenceReleasesRetainedTraffic: true,
  duplicateRouteTerminalRedelivers: false
});
assert.deepEqual(relocation.actorCapabilities.messageFollow, {
  preservesOriginalOperationId: true,
  preservesOriginalReplyRoute: true,
  sameCallResubmitsApplicationOperation: false
});
assert.deepEqual(relocation.spotCapabilities.wholeSpotBoundary, {
  frameworkExecutionMode: 'SpotWide',
  ownerAndChildVisibility: 'atomicAggregate',
  atomicAggregateOwnerVisibility: true,
  noParticipantHandlerBeforeAggregateReady: true,
  savedAndTemporaryWorkDeliveredToOriginalParticipant: true,
  membershipPreserved: true,
  individualChildOwnerPublication: false,
  memberMembershipCallbackCount: 0
});
assert.deepEqual(relocation.spotCapabilities.perActorBoundary, {
  frameworkExecutionMode: 'PerActor',
  spotShellMovesBeforeChildActors: true,
  childActorsMoveAsIndependentUnits: true,
  spotDirectTrafficHandlerTargetAfterCommit: 'targetSpotShell',
  actorTrafficHandlerTargetDuringRelocation: 'currentActorOwner',
  splitOwnershipAllowedWhileRelocationActive: true,
  splitOwnershipAllowedAtSteadyTerminal: false,
  steadyTerminalSplitOwnerCount: 0,
  spotApplicationStateTransferred: false,
  membershipPreserved: true,
  memberMembershipCallbackCount: 0
});
assert.deepEqual(relocation.spotCapabilities.instanceSpotBoundary, {
  sourceExistingInstanceRequired: true,
  missingSourceInstanceHiddenTargetCreationCount: 0,
  actorMembershipCallbackCount: 0
});
assert.deepEqual(
  relocation.adapterContract.requiredBehaviorGroups,
  requiredRelocationGroups
);
assert.deepEqual(relocation.adapterContract.observationFields, [
  'milestone',
  'profile',
  'statePolicy',
  'event',
  'operationId',
  'replyRouteId',
  'objectGeneration',
  'sourceTenure',
  'targetTenure',
  'sourceOwnerGeneration',
  'targetOwnerGeneration',
  'currentOwner',
  'routeTerminalFingerprint',
  'retainedFifoSequence',
  'callback',
  'closingReason',
  'handlerTarget',
  'deliveryCount',
  'successorAdmissionCount',
  'settlementCount',
  'cleanupCount'
]);

assert.equal(boundSessionRelocation.fixture, 'zlink.framework.bound-session-relocation');
assert.equal(boundSessionRelocation.version, 1);
assert.equal(
  boundSessionRelocation.behaviorFixture,
  'framework/runtime/conformance/relocation-behavior-v1.json'
);
assert.equal(
  boundSessionRelocation.wireGolden,
  'framework/runtime/protocol/golden/session-relocation-barrier-v1.json'
);
assert.equal(boundSessionRelocation.behaviorBranch, 'boundSessionRoute');
assert.deepEqual(boundSessionRelocation.contractProfiles, [
  'actorJoin',
  'actorHostHandoff',
  'perActorUserSpot',
  'spotWideUserSpot'
]);
assert.deepEqual(boundSessionRelocation.enabledProfiles, ['actorJoin']);
assert.deepEqual(boundSessionRelocation.eventProjection, {
  sealAccepted: 'sourceAdmissionSealed',
  callbackPushSubmitted: 'applicationOperationSubmitted',
  routeApplied: 'sessionRouteConverged',
  routeTerminal: 'sessionRouteTerminalDelivered'
});
const relocationOwnership = boundSessionRelocation.ownershipModel;
assert.equal(relocationOwnership.actorTenure.owner, 'locationAuthority');
assert.equal(relocationOwnership.actorTenure.proof, 'immutableCommittedActorTenure');
assert.equal(relocationOwnership.actorTenure.downstreamReauthorization, false);
assert.ok(relocationOwnership.actorTenure.fields.includes('ownerLeaseGeneration'));
assert.equal(relocationOwnership.actorTenure.fields.includes('storeVersion'), false);
assert.equal(relocationOwnership.sessionBinding.owner, 'sessionBindingAggregate');
for (const ownedState of [
  'acceptedHighWater',
  'activeFrames',
  'sealIdentity',
  'postSealIngress',
  'preRouteTargetOutbound',
  'committedActorRoute',
  'routeTerminalFingerprint',
  'pendingReplyClaims'
])
  assert.ok(relocationOwnership.sessionBinding.owns.includes(ownedState), ownedState);
assert.equal(relocationOwnership.sessionBinding.sealWaitsForPreSealActiveFrames, true);
assert.equal(relocationOwnership.messageFollow.owner, 'previousOwnerMessageFollowRegistry');
const exactMessageFollowLeaseRecordFields = [
  'leaseIdentity',
  'exactSourceTenure',
  'exactTargetTenure',
  'hopAndVisitedOwners',
  'retainedBudget',
  'expiry'
];
assert.deepEqual(
  relocationOwnership.messageFollow.exactLeaseRecordFields,
  exactMessageFollowLeaseRecordFields
);
const messageFollowRelayRecordFields = [
  'leaseIdentity',
  'exactSourceTenure',
  'exactTargetTenure',
  'originalOperationIdentity',
  'originalReplyCapability',
  'visitedOwners',
  'remainingBudget',
  'expiry'
];
assert.deepEqual(
  relocationOwnership.messageFollow.relayRecordFields,
  messageFollowRelayRecordFields
);
assert.equal(relocationOwnership.messageFollow.noticeCompletionRequiredForDelivery, false);
assert.equal(relocationOwnership.messageFollow.sessionRouteTerminalRemovesLease, false);
assert.equal(relocationOwnership.routeResolver.authority, false);
assert.equal(relocationOwnership.routeResolver.cacheIsHintOnly, true);
assert.equal(relocationOwnership.targetOutbound.producer, 'targetActorEmitter');
assert.equal(
  relocationOwnership.targetOutbound.producerSubmissionWindow,
  'targetPostCommitLifecycle'
);
const boundSessionPacketFenceFields = [
  'actorTenureCandidate',
  'targetNodeRid',
  'targetNodeGeneration',
  'sessionIdentity',
  'bindingGeneration'
];
assert.deepEqual(relocationOwnership.targetOutbound.producerPendingRecordFields, [
  'outboundIdentity',
  'authenticatedProducerNodeRid',
  'authenticatedProducerNodeGeneration',
  ...boundSessionPacketFenceFields,
  'payloadFingerprint'
]);
assert.equal(
  relocationOwnership.targetOutbound.outboundIdentitySource,
  'sessionBindingAggregateArrival'
);
assert.equal(relocationOwnership.targetOutbound.outboundIdentityIsWireField, false);
assert.equal(
  relocationOwnership.targetOutbound.outboundIdentitySupportsDuplicateSuppression,
  false
);
assert.equal(
  relocationOwnership.targetOutbound.preAdmissionPendingOwner,
  'sessionBindingAggregate'
);
assert.equal(
  relocationOwnership.targetOutbound.producerRetainsAfterTransportAcceptance,
  false
);
assert.equal(relocationOwnership.targetOutbound.admissionOwner, 'sessionBindingAggregate');
assert.equal(relocationOwnership.targetOutbound.admissionFence, 'boundSessionPacket');
assert.equal(relocationOwnership.targetOutbound.relocationPhaseAllowListRequired, false);
assert.deepEqual(relocationOwnership.targetOutbound.admissionEligibility, [
  'sessionAlive',
  'authenticatedProducerMatchesCandidateTarget',
  'immutableAcceptedTargetProofMatchesCandidateTenure',
  'exactSessionIdentity',
  'exactBindingGeneration',
  'nextSessionOwnerAdmissionSequence'
]);
assert.deepEqual(relocationOwnership.targetOutbound.releasedDeliveryEligibility, [
  'sessionAlive',
  'exactSessionIdentity',
  'exactBindingGeneration',
  'sessionOwnerAdmissionSequenceOrder',
  'admittedEntryOwnership',
  'actorTenureInRouteAppliedTenures'
]);
assert.equal(
  relocationOwnership.targetOutbound.identicalPayloadPolicy,
  'eachAuthenticatedOneWayArrivalIsADistinctAdmission'
);
assert.equal(
  relocationOwnership.targetOutbound.immutableTargetProofAcceptedBeforeAdmission,
  true
);
assert.equal(
  relocationOwnership.targetOutbound.admissionMayCompleteAfterExactRouteTerminal,
  true
);
assert.equal(
  relocationOwnership.targetOutbound.staleAdmissionSettlement,
  'rejectAndSettleWithoutDelivery'
);
assert.equal(relocationOwnership.targetOutbound.admissionMayCompleteAfterRouteApply, true);
assert.equal(
  relocationOwnership.targetOutbound.retentionOwnerAfterAdmission,
  'sessionBindingAggregate'
);
assert.equal(relocationOwnership.targetOutbound.producerRetainsAfterAdmission, false);
assert.equal(relocationOwnership.targetOutbound.retentionOwnerMayReauthorizeActorTenure, false);
assert.equal(relocationOwnership.targetOutbound.fifoOwner, 'sessionBindingAggregate');
assert.equal(
  relocationOwnership.targetOutbound.fifoSequenceSource,
  'sessionOwnerAdmissionOrder'
);
assert.equal(relocationOwnership.targetOutbound.fifoSequenceBeginsAt, 'sessionOwnerAdmission');
assert.equal(relocationOwnership.targetOutbound.physicalDeliveryBeforeRouteApply, false);
assert.equal(
  relocationOwnership.targetOutbound.releaseAfterAtomicRouteApply,
  'fifoExactlyOnce'
);
assert.equal(relocationOwnership.routeConvergence.detachedBestEffortAllowed, false);
assert.equal(relocationOwnership.routeConvergence.exactRouteTerminalGatesSuccessor, true);
assert.equal(
  relocationOwnership.routeConvergence.routeApplyMayReleaseTrafficBeforeExactTerminal,
  true
);
assert.deepEqual(relocationOwnership.targetProofAcquisition, {
  localCommittedProofStoreReadCount: 0,
  remoteMissingProofStoreReadCount: 1,
  identicalConcurrentRemoteRequestsShareSingleFlight: true,
  missingRemoteProofFailsBeforeAggregateMutation: true
});
const milestones = boundSessionRelocation.supportMilestones;
assert.deepEqual(Object.keys(milestones), ['M0', 'M1', 'M2', 'M3']);
for (const milestone of ['M0', 'M1', 'M2'])
  assert.deepEqual(milestones[milestone].profiles, ['actorJoin']);
const milestoneMetadataFields = new Set(['profiles', 'inherits', 'overrides', 'status']);
const sameJsonValue = (left, right) => JSON.stringify(left) === JSON.stringify(right);
const effectiveMilestones = new Map();
const resolvingMilestones = new Set();
const resolveEffectiveMilestone = (name) => {
  if (effectiveMilestones.has(name)) return effectiveMilestones.get(name);
  const milestone = milestones[name];
  assert.ok(milestone, `${name}: unknown milestone`);
  assert.notEqual(milestone.status, 'deferred', `${name}: deferred milestone is not executable`);
  assert.equal(resolvingMilestones.has(name), false, `${name}: inheritance cycle`);
  resolvingMilestones.add(name);
  const parent = milestone.inherits
    ? resolveEffectiveMilestone(milestone.inherits)
    : { profiles: milestone.profiles, constraints: {} };
  assert.deepEqual(
    milestone.profiles,
    parent.profiles,
    `${name}: inherited profiles must remain explicit and unchanged`
  );
  const overrides = milestone.overrides ?? [];
  assert.equal(new Set(overrides).size, overrides.length, `${name}: duplicate override`);
  const constraints = structuredClone(parent.constraints);
  for (const [field, value] of Object.entries(milestone)) {
    if (milestoneMetadataFields.has(field)) continue;
    if (Object.hasOwn(parent.constraints, field) && !sameJsonValue(parent.constraints[field], value))
      assert.ok(overrides.includes(field), `${name}: ${field} changes without an explicit override`);
    constraints[field] = structuredClone(value);
  }
  for (const field of overrides) {
    assert.ok(Object.hasOwn(parent.constraints, field), `${name}: override has no inherited field ${field}`);
    assert.ok(Object.hasOwn(milestone, field), `${name}: override has no replacement ${field}`);
    assert.equal(
      sameJsonValue(parent.constraints[field], milestone[field]),
      false,
      `${name}: override does not change ${field}`
    );
  }
  resolvingMilestones.delete(name);
  const effective = { profiles: structuredClone(milestone.profiles), constraints };
  effectiveMilestones.set(name, effective);
  return effective;
};
for (const milestone of ['M0', 'M1', 'M2']) resolveEffectiveMilestone(milestone);
assert.equal(milestones.M0.boundSessionCount, 0);
assert.equal(milestones.M0.positiveRouteCacheMaxAgeMilliseconds, 0);
assert.equal(milestones.M0.positiveRouteCacheAllowed, false);
assert.equal(milestones.M0.requiresQuiescentApplicationTraffic, true);
assert.deepEqual(milestones.M0.allowedTraffic, []);
assert.equal(milestones.M0.existingMessageFollowLeaseCount, 0);
assert.equal(milestones.M0.newExactMessageFollowLeaseRequired, true);
assert.equal(milestones.M0.successorRelocationAllowed, false);
assert.equal(milestones.M1.inherits, 'M0');
assert.deepEqual(milestones.M1.overrides, [
  'allowedTraffic',
  'boundSessionCount',
  'requiresQuiescentApplicationTraffic'
]);
assert.equal(milestones.M1.boundSessionCount, 1);
assert.equal(milestones.M1.requiresQuiescentApplicationTraffic, false);
assert.deepEqual(milestones.M1.allowedTraffic, [
  'preSealAcceptedActiveFrame',
  'postSealBoundSessionIngress',
  'targetPostCommitLifecycleOutboundOneWay'
]);
assert.equal(milestones.M1.activeReplyBacklogCount, 0);
assert.equal(milestones.M1.concurrentBindUnbindCloseAllowed, false);
assert.equal(milestones.M1.sessionOwnerOwnsIngressAndHighWater, true);
assert.equal(milestones.M1.sessionOwnerOwnsPostCommitPreRouteOutbound, true);
assert.equal(milestones.M1.receiverRequiresFullImmutableFence, true);
assert.equal(milestones.M1.receiverMayReauthorizeAgainstLocalMirrors, false);
assert.equal(milestones.M2.inherits, 'M1');
assert.deepEqual(milestones.M2.overrides, [
  'allowedTraffic',
  'positiveRouteCacheAllowed',
  'positiveRouteCacheMaxAgeMilliseconds',
  'successorRelocationAllowed'
]);
assert.deepEqual(milestones.M2.allowedTraffic, [
  'preSealAcceptedActiveFrame',
  'postSealBoundSessionIngress',
  'targetPostCommitLifecycleOutboundOneWay',
  'boundActorRequestReply'
]);
assert.equal(milestones.M2.successorAdmissionAfterPredecessorRouteTerminal, true);
assert.equal(milestones.M2.sameObjectGenerationAllowed, true);
assert.equal(milestones.M2.distinctOwnerTenuresRequired, true);
assert.equal(milestones.M2.positiveRouteCacheAllowed, true);
assert.equal(milestones.M2.positiveRouteCacheMaxAgeMilliseconds, null);
assert.equal(milestones.M2.delayedNoticeCorrectnessRequired, true);
assert.deepEqual(
  ['M0', 'M1', 'M2'].map(name => ({
    name,
    boundSessionCount: effectiveMilestones.get(name).constraints.boundSessionCount,
    requiresQuiescentApplicationTraffic:
      effectiveMilestones.get(name).constraints.requiresQuiescentApplicationTraffic,
    allowedTraffic: effectiveMilestones.get(name).constraints.allowedTraffic,
    successorRelocationAllowed:
      effectiveMilestones.get(name).constraints.successorRelocationAllowed,
    positiveRouteCacheAllowed:
      effectiveMilestones.get(name).constraints.positiveRouteCacheAllowed,
    positiveRouteCacheMaxAgeMilliseconds:
      effectiveMilestones.get(name).constraints.positiveRouteCacheMaxAgeMilliseconds
  })),
  [
    {
      name: 'M0',
      boundSessionCount: 0,
      requiresQuiescentApplicationTraffic: true,
      allowedTraffic: [],
      successorRelocationAllowed: false,
      positiveRouteCacheAllowed: false,
      positiveRouteCacheMaxAgeMilliseconds: 0
    },
    {
      name: 'M1',
      boundSessionCount: 1,
      requiresQuiescentApplicationTraffic: false,
      allowedTraffic: [
        'preSealAcceptedActiveFrame',
        'postSealBoundSessionIngress',
        'targetPostCommitLifecycleOutboundOneWay'
      ],
      successorRelocationAllowed: false,
      positiveRouteCacheAllowed: false,
      positiveRouteCacheMaxAgeMilliseconds: 0
    },
    {
      name: 'M2',
      boundSessionCount: 1,
      requiresQuiescentApplicationTraffic: false,
      allowedTraffic: [
        'preSealAcceptedActiveFrame',
        'postSealBoundSessionIngress',
        'targetPostCommitLifecycleOutboundOneWay',
        'boundActorRequestReply'
      ],
      successorRelocationAllowed: true,
      positiveRouteCacheAllowed: true,
      positiveRouteCacheMaxAgeMilliseconds: null
    }
  ]
);
assert.deepEqual(boundSessionRelocation.modelExecutionPolicy, {
  effectiveConstraintsAppliedToActions: [
    'boundSessionCount',
    'maxActiveRelocationsPerLogicalObject',
    'requiresQuiescentApplicationTraffic',
    'allowedTraffic',
    'existingMessageFollowLeaseCount',
    'newExactMessageFollowLeaseRequired',
    'positiveRouteCacheMaxAgeMilliseconds',
    'positiveRouteCacheAllowed',
    'successorRelocationAllowed'
  ],
  environmentPreconditionsValidatedButNotSynthesized: [
    'activeReplyBacklogCount',
    'requiresHealthySameVersionPeers',
    'restartFallbackEnabled'
  ],
  supportedTrafficComesFromEffectiveMilestone: true
});
for (const name of ['M0', 'M1', 'M2']) {
  const effective = effectiveMilestones.get(name).constraints;
  for (const field of [
    ...boundSessionRelocation.modelExecutionPolicy.effectiveConstraintsAppliedToActions,
    ...boundSessionRelocation.modelExecutionPolicy.environmentPreconditionsValidatedButNotSynthesized
  ])
    assert.ok(Object.hasOwn(effective, field), `${name}: missing effective constraint ${field}`);
  assert.equal(
    effective.requiresQuiescentApplicationTraffic,
    effective.allowedTraffic.length === 0,
    `${name}: quiescence and supported traffic disagree`
  );
  assert.equal(effective.activeReplyBacklogCount, 0, `${name}: active reply backlog`);
  assert.equal(effective.requiresHealthySameVersionPeers, true, `${name}: peer version/health`);
  assert.equal(effective.restartFallbackEnabled, false, `${name}: restart fallback`);
}
assert.equal(milestones.M3.status, 'deferred');
assert.deepEqual(milestones.M3.profiles, [
  'actorHostHandoff',
  'perActorUserSpot',
  'spotWideUserSpot',
  'instanceSpot'
]);
assert.deepEqual(milestones.M3.deferredVariants, ['sessionOwnerIsRelocationTarget']);
assert.deepEqual(
  new Set([...boundSessionRelocation.enabledProfiles, ...milestones.M3.profiles]).size,
  boundSessionRelocation.enabledProfiles.length + milestones.M3.profiles.length
);
assert.equal(boundSessionRelocation.transitionModel.preCommitAbortReturnsTo, 'stableSource');
assert.equal(boundSessionRelocation.transitionModel.postCommitRollbackAllowed, false);
assert.equal(boundSessionRelocation.transitionModel.maxActiveBarriersPerLogicalObject, 1);
assert.equal(boundSessionRelocation.transitionModel.successorStartsBeforePredecessorTerminal, false);
assert.equal(
  boundSessionRelocation.transitionModel
    .successorEventuallyStartsAfterPredecessorTerminalWhenRuntimeAndSessionRemainAvailable,
  true
);
assert.equal(boundSessionRelocation.transitionModel.duplicateTransitionMutatesState, false);
const transitionStates = new Set(boundSessionRelocation.transitionModel.states);
for (const [profile, transitions] of [
  ['bound', boundSessionRelocation.transitionModel.boundSessionTransitions],
  ['unbound', boundSessionRelocation.transitionModel.unboundTransitions]
]) {
  const edges = new Set();
  for (const [from, to] of transitions) {
    assert.ok(transitionStates.has(from), `${profile}: unknown source state ${from}`);
    assert.ok(transitionStates.has(to), `${profile}: unknown target state ${to}`);
    assert.ok(!edges.has(`${from}\0${to}`), `${profile}: duplicate transition`);
    edges.add(`${from}\0${to}`);
  }
}
for (const state of boundSessionRelocation.transitionModel.shutdownAllowedFrom)
  assert.ok(transitionStates.has(state), `shutdown: unknown state ${state}`);
const boundTransitionKeys = new Set(
  boundSessionRelocation.transitionModel.boundSessionTransitions
    .map(([from, to]) => `${from}\0${to}`)
);
for (const transition of [
  ['authorityCompleted', 'routeConverging'],
  ['routeConverging', 'routeAppliedAwaitingTerminal'],
  ['routeAppliedAwaitingTerminal', 'stableTarget']
])
  assert.ok(boundTransitionKeys.has(`${transition[0]}\0${transition[1]}`), transition.join(' -> '));
const unboundTransitionKeys = new Set(
  boundSessionRelocation.transitionModel.unboundTransitions
    .map(([from, to]) => `${from}\0${to}`)
);
assert.ok(unboundTransitionKeys.has('authorityCompleted\0stableTarget'));

const modelRecords = boundSessionRelocation.modelRecords;
assert.deepEqual(modelRecords.sessionBinding, {
  sessionIdentity: 'session-actor-1',
  bindingGeneration: 1
});
const routeTerminalFenceFields = [
  'relocationIdentity',
  'actorTenure',
  'sessionIdentity',
  'bindingGeneration',
  'sealIdentity',
  'acceptedHighWater',
  'actionAndResult',
  'terminalFingerprint'
];
const expectedRouteTerminalRecord = (relocationIdentity) => {
  const relocationRecord = modelRecords.relocations[relocationIdentity];
  assert.ok(relocationRecord, `${relocationIdentity}: unknown relocation record`);
  return {
    relocationIdentity,
    actorTenure: relocationRecord.targetTenure,
    sessionIdentity: modelRecords.sessionBinding.sessionIdentity,
    bindingGeneration: modelRecords.sessionBinding.bindingGeneration,
    sealIdentity: relocationRecord.sealIdentity,
    acceptedHighWater: relocationRecord.acceptedHighWater,
    actionAndResult: relocationRecord.actionAndResult,
    terminalFingerprint: relocationRecord.routeTerminalFingerprint
  };
};
const exactLeasePairs = new Set();
for (const [leaseIdentity, leaseRecord] of Object.entries(modelRecords.messageFollowLeases)) {
  assert.deepEqual(Object.keys(leaseRecord), exactMessageFollowLeaseRecordFields, leaseIdentity);
  assert.equal(leaseRecord.leaseIdentity, leaseIdentity, leaseIdentity);
  assert.notEqual(leaseRecord.exactSourceTenure, leaseRecord.exactTargetTenure, leaseIdentity);
  assert.ok(leaseRecord.hopAndVisitedOwners.length > 0, leaseIdentity);
  assert.ok(Number.isSafeInteger(leaseRecord.retainedBudget) && leaseRecord.retainedBudget > 0, leaseIdentity);
  assert.ok(Number.isSafeInteger(leaseRecord.expiry) && leaseRecord.expiry > 0, leaseIdentity);
  const pair = `${leaseRecord.exactSourceTenure}\0${leaseRecord.exactTargetTenure}`;
  assert.equal(exactLeasePairs.has(pair), false, `${leaseIdentity}: duplicate exact tenure pair`);
  exactLeasePairs.add(pair);
}
const relocationModelClock = 10;
for (const [candidateName, candidate] of
  Object.entries(modelRecords.messageFollowRelayCandidates)) {
  assert.deepEqual(Object.keys(candidate), messageFollowRelayRecordFields, candidateName);
  const lease = modelRecords.messageFollowLeases[candidate.leaseIdentity];
  assert.ok(lease, `${candidateName}: unknown Message Follow lease`);
  assert.equal(candidate.exactSourceTenure, lease.exactSourceTenure, candidateName);
  assert.equal(candidate.exactTargetTenure, lease.exactTargetTenure, candidateName);
  assert.ok(candidate.originalOperationIdentity.length > 0, candidateName);
  assert.ok(candidate.originalReplyCapability.length > 0, candidateName);
  assert.ok(candidate.visitedOwners.length > 0, candidateName);
  assert.ok(Number.isSafeInteger(candidate.remainingBudget), candidateName);
  assert.ok(Number.isSafeInteger(candidate.expiry) && candidate.expiry > 0, candidateName);
  assert.ok(candidate.expiry <= lease.expiry, `${candidateName}: extends lease expiry`);
}
const exactRelayCandidate = modelRecords.messageFollowRelayCandidates['relay-1-exact'];
assert.equal(new Set(exactRelayCandidate.visitedOwners).size, exactRelayCandidate.visitedOwners.length);
assert.equal(exactRelayCandidate.visitedOwners.includes('ownerB'), false);
assert.ok(exactRelayCandidate.remainingBudget > 0);
assert.ok(exactRelayCandidate.expiry > relocationModelClock);
assert.equal(
  modelRecords.messageFollowRelayCandidates['relay-1-target-visited']
    .visitedOwners.includes('ownerB'),
  true
);
const duplicateVisitedCandidate =
  modelRecords.messageFollowRelayCandidates['relay-1-duplicate-visited'];
assert.ok(new Set(duplicateVisitedCandidate.visitedOwners).size
  < duplicateVisitedCandidate.visitedOwners.length);
assert.equal(
  modelRecords.messageFollowRelayCandidates['relay-1-budget-exhausted'].remainingBudget,
  0
);
assert.ok(
  modelRecords.messageFollowRelayCandidates['relay-1-expired'].expiry <= relocationModelClock
);
for (const [relocationIdentity, relocationRecord] of Object.entries(modelRecords.relocations)) {
  assert.ok(relocationRecord.targetOwner.length > 0, relocationIdentity);
  assert.ok(relocationRecord.targetTenure.startsWith(`${relocationRecord.targetOwner}#`), relocationIdentity);
  assert.ok(relocationRecord.targetNodeRid.length > 0, relocationIdentity);
  assert.ok(
    Number.isSafeInteger(relocationRecord.targetNodeGeneration)
      && relocationRecord.targetNodeGeneration > 0,
    relocationIdentity
  );
  assert.ok(relocationRecord.sealIdentity.length > 0, relocationIdentity);
  assert.ok(Number.isSafeInteger(relocationRecord.acceptedHighWater), relocationIdentity);
  const lease = modelRecords.messageFollowLeases[relocationRecord.messageFollowLeaseIdentity];
  assert.ok(lease, `${relocationIdentity}: missing exact Message Follow lease`);
  assert.equal(lease.exactTargetTenure, relocationRecord.targetTenure, relocationIdentity);
  const exactCandidate = modelRecords.routeTerminalCandidates[`${relocationIdentity.replace('relocation-', 'terminal-')}-exact`];
  assert.deepEqual(exactCandidate, expectedRouteTerminalRecord(relocationIdentity), relocationIdentity);
}
for (const [name, candidate] of Object.entries(modelRecords.routeTerminalCandidates)) {
  assert.deepEqual(Object.keys(candidate), routeTerminalFenceFields, name);
  assert.ok(modelRecords.relocations[candidate.relocationIdentity], `${name}: unknown relocation`);
}
assert.deepEqual(modelRecords.routeTerminalCandidates['terminal-1-wrong-binding'], {
  ...expectedRouteTerminalRecord('relocation-1'),
  bindingGeneration: 0
});
assert.deepEqual(modelRecords.routeTerminalCandidates['terminal-1-conflict'], {
  ...expectedRouteTerminalRecord('relocation-1'),
  terminalFingerprint: 'terminal-conflict'
});

const runRelocationModel = (scenario) => {
  const effectiveMilestone = effectiveMilestones.get(scenario.milestone);
  assert.ok(effectiveMilestone, `${scenario.name}: milestone is not executable`);
  assert.ok(
    effectiveMilestone.profiles.includes(scenario.profile),
    `${scenario.name}: ${scenario.profile} is outside ${scenario.milestone}`
  );
  assert.ok(
    boundSessionRelocation.enabledProfiles.includes(scenario.profile),
    `${scenario.name}: ${scenario.profile} is not enabled`
  );
  const constraints = effectiveMilestone.constraints;
  const model = {
    state: 'stableSource',
    currentOwner: 'ownerA',
    currentTenure: 'ownerA#0',
    boundSessionCount: constraints.boundSessionCount,
    sessionAlive: constraints.boundSessionCount === 1,
    activeBarrierCount: 0,
    activeFrameCount: 0,
    acceptedActiveFrameCount: 0,
    completedActiveFrameCount: 0,
    sealWaitingObservationCount: 0,
    queuedSuccessorCount: 0,
    producerPendingOutboundCount: 0,
    emitterSubmissionOrder: [],
    outboundAdmissionOrder: [],
    outboundAdmissionAcceptedCount: 0,
    transportAdmissionRejectCounts: {},
    transportRejectedSettlementOrder: [],
    targetProofPendingObservationCount: 0,
    acceptedTargetProofCount: 0,
    pendingCurrentActorTenureValidationCount: 0,
    admittedActorTenureReauthorizationCount: 0,
    outboundAdmissionRejectCounts: {},
    producerRejectedSettlementOrder: [],
    retainedOutboundCount: 0,
    releasedOutboundCount: 0,
    retainedIngressCount: 0,
    releasedIngressCount: 0,
    preCommitUnavailableCount: 0,
    outboundDeliveryOrder: [],
    ingressDeliveryOrder: [],
    outboundSettlementOrder: [],
    ingressSettlementOrder: [],
    outboundFailureOrder: [],
    ingressFailureOrder: [],
    routeAttemptCount: 0,
    routeApplyCount: 0,
    duplicateRouteApplyCount: 0,
    routeApplyConflictCount: 0,
    routeTerminalCount: 0,
    routeTerminalFenceRejectCount: 0,
    duplicateTerminalCount: 0,
    routeTerminalConflictCount: 0,
    duplicateAbortCount: 0,
    abortConflictCount: 0,
    shutdownCount: 0,
    duplicateShutdownCount: 0,
    messageFollowLeaseCount: 0,
    messageFollowLeaseIdentities: [],
    messageFollowRelayKeys: [],
    messageFollowRelayAcceptedCount: 0,
    messageFollowRelayRejectCounts: {},
    followedRequestOwners: [],
    postRoundTripRequestOwners: [],
    routeCacheTenure: constraints.positiveRouteCacheAllowed ? 'ownerA#0' : undefined,
    routeCachePresent: constraints.positiveRouteCacheAllowed,
    routeCacheRefreshCount: 0,
    routeCacheRemovalCount: 0,
    routeCachePreserveCount: 0,
    successorBlockedObservationCount: 0,
    producerPendingFailureOrder: []
  };
  let tenureOrdinal = 0;
  let pendingTargetOwner;
  let sourceTenure;
  let activeRelocationIdentity;
  let routeApplyFingerprint;
  let acceptedRouteTerminalRecord;
  let abortFingerprint;
  let routeCacheTenure = model.routeCacheTenure;
  const queuedSuccessors = [];
  const messageFollowLeases = new Map();
  const activeFrames = [];
  const producerPendingOutbound = new Map();
  const admittedOutboundRecords = new Map();
  const outboundQueueEntries = new Map();
  const acceptedTargetProofs = new Map();
  const routeAppliedTenures = new Set();
  let lastAdmittedSequence = 0;
  let lastDeliveredSequence = 0;
  const retainedOutbound = [];
  const releasedOutbound = [];
  const retainedIngress = [];
  const releasedIngress = [];
  assert.equal(
    messageFollowLeases.size,
    constraints.existingMessageFollowLeaseCount,
    `${scenario.name}: initial Message Follow lease count`
  );
  const requireState = (expected, action) =>
    assert.ok(expected.includes(model.state), `${scenario.name}: ${action} from ${model.state}`);
  const evaluateMessageFollowRelay = (
    candidateName,
    expectedOperationIdentity,
    expectedReplyCapability
  ) => {
    const candidate = modelRecords.messageFollowRelayCandidates[candidateName];
    assert.ok(candidate, `${scenario.name}: unknown Message Follow relay candidate`);
    const lease = messageFollowLeases.get(candidate.leaseIdentity);
    if (!lease) return 'unknownLease';
    if (candidate.exactSourceTenure !== lease.exactSourceTenure) return 'sourceTenureMismatch';
    if (candidate.exactTargetTenure !== lease.exactTargetTenure) return 'targetTenureMismatch';
    if (expectedOperationIdentity
      && candidate.originalOperationIdentity !== expectedOperationIdentity)
      return 'operationIdentityMismatch';
    if (expectedReplyCapability
      && candidate.originalReplyCapability !== expectedReplyCapability)
      return 'replyCapabilityMismatch';
    if (!candidate.originalOperationIdentity) return 'missingOperationIdentity';
    if (!candidate.originalReplyCapability) return 'missingReplyCapability';
    if (new Set(candidate.visitedOwners).size !== candidate.visitedOwners.length)
      return 'duplicateVisitedOwner';
    const sourceOwner = candidate.exactSourceTenure.split('#')[0];
    const targetOwner = candidate.exactTargetTenure.split('#')[0];
    if (!candidate.visitedOwners.includes(sourceOwner)) return 'sourceOwnerNotVisited';
    if (candidate.visitedOwners.includes(targetOwner)) return 'targetAlreadyVisited';
    if (candidate.remainingBudget <= 0) return 'budgetExhausted';
    if (candidate.remainingBudget >= lease.retainedBudget) return 'budgetNotConsumed';
    if (candidate.expiry > lease.expiry) return 'expiryExceedsLease';
    if (candidate.expiry <= relocationModelClock) return 'expired';
    if (routeCacheTenure !== candidate.exactSourceTenure) return 'routeCacheSourceMismatch';
    if (model.currentTenure !== candidate.exactTargetTenure) return 'currentTargetMismatch';
    return 'accepted';
  };
  const parseBoundSessionOutboundRecord = (parameters, action) => {
    assert.equal(parameters.length, 9, `${scenario.name}: ${action} fence field count`);
    const [
      outboundIdentity,
      authenticatedProducerNodeRid,
      authenticatedProducerNodeGenerationText,
      actorTenureCandidate,
      targetNodeRid,
      targetNodeGenerationText,
      sessionIdentity,
      bindingGenerationText,
      payloadFingerprint
    ] = parameters;
    const authenticatedProducerNodeGeneration = Number(
      authenticatedProducerNodeGenerationText
    );
    const targetNodeGeneration = Number(targetNodeGenerationText);
    const bindingGeneration = Number(bindingGenerationText);
    assert.ok(outboundIdentity, `${scenario.name}: missing outbound identity`);
    assert.ok(authenticatedProducerNodeRid, `${scenario.name}: missing authenticated producer RID`);
    assert.ok(
      Number.isSafeInteger(authenticatedProducerNodeGeneration)
        && authenticatedProducerNodeGeneration > 0,
      `${scenario.name}: authenticated producer node generation`
    );
    assert.ok(actorTenureCandidate, `${scenario.name}: missing Actor tenure candidate`);
    assert.ok(targetNodeRid, `${scenario.name}: missing target node RID`);
    assert.ok(
      Number.isSafeInteger(targetNodeGeneration) && targetNodeGeneration > 0,
      `${scenario.name}: target node generation`
    );
    assert.ok(sessionIdentity, `${scenario.name}: missing Session identity`);
    assert.ok(Number.isSafeInteger(bindingGeneration), `${scenario.name}: binding generation`);
    assert.ok(payloadFingerprint, `${scenario.name}: missing payload fingerprint`);
    const record = {
      outboundIdentity,
      authenticatedProducerNodeRid,
      authenticatedProducerNodeGeneration,
      actorTenureCandidate,
      targetNodeRid,
      targetNodeGeneration,
      sessionIdentity,
      bindingGeneration,
      payloadFingerprint
    };
    assert.deepEqual(
      Object.keys(record).slice(3, 8),
      boundSessionPacketFenceFields,
      `${scenario.name}: boundSessionPacket fence projection`
    );
    return record;
  };
  const evaluateAuthenticatedProducer = (record) =>
    record.authenticatedProducerNodeRid === record.targetNodeRid
      && record.authenticatedProducerNodeGeneration === record.targetNodeGeneration
      ? 'accepted'
      : 'authenticatedProducerNodeMismatch';
  const evaluateSessionOwnerAdmission = (record) => {
    if (!model.sessionAlive) return 'sessionClosed';
    const pending = producerPendingOutbound.get(record.outboundIdentity);
    if (!pending) return 'noProducerPendingRecord';
    if (!sameJsonValue(pending, record)) return 'producerPendingRecordMismatch';
    const producerResult = evaluateAuthenticatedProducer(record);
    if (producerResult !== 'accepted') return producerResult;
    if (record.sessionIdentity !== modelRecords.sessionBinding.sessionIdentity)
      return 'staleSessionIdentity';
    if (record.bindingGeneration !== modelRecords.sessionBinding.bindingGeneration)
      return 'staleBindingGeneration';
    const acceptedProof = acceptedTargetProofs.get(record.actorTenureCandidate);
    if (acceptedProof === undefined) return 'targetProofPending';
    if (acceptedProof.targetNodeRid !== record.targetNodeRid
      || acceptedProof.targetNodeGeneration !== record.targetNodeGeneration)
      return 'targetProofMismatch';
    model.pendingCurrentActorTenureValidationCount += 1;
    if (record.actorTenureCandidate !== model.currentTenure) return 'staleActorTenure';
    return 'accepted';
  };
  const evaluateReleasedDelivery = (outboundIdentity) => {
    if (!model.sessionAlive) return 'sessionClosed';
    const admitted = admittedOutboundRecords.get(outboundIdentity);
    if (!admitted) return 'noAdmittedEntry';
    const queuedEntry = outboundQueueEntries.get(outboundIdentity);
    if (!queuedEntry) return 'noReleasedEntry';
    if (!sameJsonValue(queuedEntry, admitted)) return 'releasedEntryMismatch';
    if (admitted.sessionIdentity !== modelRecords.sessionBinding.sessionIdentity)
      return 'staleSessionIdentity';
    if (admitted.bindingGeneration !== modelRecords.sessionBinding.bindingGeneration)
      return 'staleBindingGeneration';
    if (!routeAppliedTenures.has(admitted.actorTenureCandidate)) return 'routeNotAppliedForTenure';
    if (admitted.admissionSequence !== lastDeliveredSequence + 1)
      return 'admissionSequenceMismatch';
    return 'deliver';
  };
  for (const action of scenario.actions) {
    const [verb, ...parameters] = action.split(':');
    switch (verb) {
      case 'acceptPreSealActiveFrame': {
        requireState(['stableSource', 'stableTarget'], action);
        assert.equal(constraints.boundSessionCount, 1, `${scenario.name}: ActiveFrame without session`);
        assert.ok(
          constraints.allowedTraffic.includes('preSealAcceptedActiveFrame'),
          `${scenario.name}: pre-seal ActiveFrame is outside the effective milestone`
        );
        const frameIdentity = parameters[0];
        assert.ok(frameIdentity, `${scenario.name}: missing ActiveFrame identity`);
        assert.equal(activeFrames.includes(frameIdentity), false, `${scenario.name}: duplicate ActiveFrame`);
        activeFrames.push(frameIdentity);
        model.activeFrameCount = activeFrames.length;
        model.acceptedActiveFrameCount += 1;
        break;
      }
      case 'beginRelocation':
        requireState(['stableSource', 'stableTarget'], action);
        assert.equal(model.activeBarrierCount, 0, `${scenario.name}: overlapping barrier`);
        assert.ok(parameters[0], `${scenario.name}: missing relocation identity`);
        assert.ok(parameters[1], `${scenario.name}: missing target owner`);
        assert.ok(modelRecords.relocations[parameters[0]], `${scenario.name}: unknown relocation record`);
        assert.equal(
          modelRecords.relocations[parameters[0]].targetOwner,
          parameters[1],
          `${scenario.name}: relocation target does not match its immutable record`
        );
        assert.notEqual(parameters[1], model.currentOwner, `${scenario.name}: same owner target`);
        activeRelocationIdentity = parameters[0];
        pendingTargetOwner = parameters[1];
        sourceTenure = model.currentTenure;
        model.state = 'sealing';
        model.activeBarrierCount = 1;
        break;
      case 'observeSealWaitingForActiveFrames':
        requireState(['sealing'], action);
        assert.ok(activeFrames.length > 0, `${scenario.name}: seal has no ActiveFrame to await`);
        model.sealWaitingObservationCount += 1;
        break;
      case 'completePreSealActiveFrame': {
        requireState(['sealing'], action);
        const frameIdentity = parameters[0];
        const frameIndex = activeFrames.indexOf(frameIdentity);
        assert.notEqual(frameIndex, -1, `${scenario.name}: unknown ActiveFrame completion`);
        activeFrames.splice(frameIndex, 1);
        model.activeFrameCount = activeFrames.length;
        model.completedActiveFrameCount += 1;
        break;
      }
      case 'commitTarget':
        requireState(['sealing'], action);
        assert.equal(activeFrames.length, 0, `${scenario.name}: seal passed an active pre-seal frame`);
        assert.ok(pendingTargetOwner, `${scenario.name}: no target owner`);
        tenureOrdinal += 1;
        model.currentOwner = pendingTargetOwner;
        model.currentTenure = `${pendingTargetOwner}#${tenureOrdinal}`;
        assert.equal(
          model.currentTenure,
          modelRecords.relocations[activeRelocationIdentity].targetTenure,
          `${scenario.name}: committed target tenure differs from its immutable record`
        );
        if (constraints.newExactMessageFollowLeaseRequired) {
          const leaseIdentity =
            modelRecords.relocations[activeRelocationIdentity].messageFollowLeaseIdentity;
          const lease = modelRecords.messageFollowLeases[leaseIdentity];
          assert.ok(lease, `${scenario.name}: missing Message Follow lease record`);
          assert.equal(lease.exactSourceTenure, sourceTenure, `${scenario.name}: lease source tenure`);
          assert.equal(
            lease.exactTargetTenure,
            model.currentTenure,
            `${scenario.name}: lease target tenure`
          );
          assert.equal(
            lease.hopAndVisitedOwners[0],
            sourceTenure.split('#')[0],
            `${scenario.name}: lease previous owner`
          );
          assert.equal(messageFollowLeases.has(leaseIdentity), false, `${scenario.name}: duplicate lease`);
          for (const existing of messageFollowLeases.values())
            assert.equal(
              existing.exactSourceTenure === lease.exactSourceTenure
                && existing.exactTargetTenure === lease.exactTargetTenure,
              false,
              `${scenario.name}: duplicate exact Message Follow tenure pair`
            );
          messageFollowLeases.set(leaseIdentity, structuredClone(lease));
          model.messageFollowLeaseCount = messageFollowLeases.size;
          model.messageFollowLeaseIdentities = [...messageFollowLeases.keys()];
        }
        model.state = 'targetCommitted';
        break;
      case 'publishTargetReady':
        requireState(['targetCommitted'], action);
        model.state = 'targetReady';
        break;
      case 'completeAuthority':
        requireState(['targetReady'], action);
        model.state = 'authorityCompleted';
        break;
      case 'beginRouteConvergence':
        requireState(['authorityCompleted'], action);
        assert.equal(constraints.boundSessionCount, 1, `${scenario.name}: bound route without session`);
        {
          const relocationRecord = modelRecords.relocations[activeRelocationIdentity];
          acceptedTargetProofs.set(model.currentTenure, {
            actorTenure: model.currentTenure,
            targetNodeRid: relocationRecord.targetNodeRid,
            targetNodeGeneration: relocationRecord.targetNodeGeneration
          });
        }
        model.acceptedTargetProofCount = acceptedTargetProofs.size;
        model.state = 'routeConverging';
        break;
      case 'completeUnboundRelocation':
        requireState(['authorityCompleted'], action);
        assert.equal(constraints.boundSessionCount, 0, `${scenario.name}: unbound completion with session`);
        model.state = 'stableTarget';
        model.activeBarrierCount = 0;
        break;
      case 'submitPreCommitOutbound':
        requireState(['sealing'], action);
        assert.equal(constraints.boundSessionCount, 1, `${scenario.name}: outbound without session`);
        model.preCommitUnavailableCount += 1;
        break;
      case 'targetEmitterSubmitsLifecycleOutbound': {
        requireState(['targetCommitted'], action);
        assert.equal(constraints.boundSessionCount, 1, `${scenario.name}: outbound without session`);
        assert.ok(
          constraints.allowedTraffic.includes('targetPostCommitLifecycleOutboundOneWay'),
          `${scenario.name}: target lifecycle outbound is outside the effective milestone`
        );
        const record = parseBoundSessionOutboundRecord(parameters, action);
        assert.equal(
          evaluateAuthenticatedProducer(record),
          'accepted',
          `${scenario.name}: target outbound transport producer mismatch`
        );
        assert.equal(
          record.sessionIdentity,
          modelRecords.sessionBinding.sessionIdentity,
          `${scenario.name}: submit Session identity`
        );
        assert.equal(
          record.bindingGeneration,
          modelRecords.sessionBinding.bindingGeneration,
          `${scenario.name}: submit binding generation`
        );
        assert.ok(
          !producerPendingOutbound.has(record.outboundIdentity)
            && !retainedOutbound.includes(record.outboundIdentity)
            && !releasedOutbound.includes(record.outboundIdentity)
            && !admittedOutboundRecords.has(record.outboundIdentity),
          `${scenario.name}: duplicate outbound id`
        );
        producerPendingOutbound.set(record.outboundIdentity, record);
        model.producerPendingOutboundCount = producerPendingOutbound.size;
        model.emitterSubmissionOrder.push(record.outboundIdentity);
        break;
      }
      case 'rejectTargetOutboundTransport': {
        requireState(['targetCommitted'], action);
        const record = parseBoundSessionOutboundRecord(parameters.slice(0, 9), action);
        const expectedReason = parameters[9];
        const reason = evaluateAuthenticatedProducer(record);
        assert.notEqual(reason, 'accepted', `${scenario.name}: exact producer transport rejected`);
        assert.equal(reason, expectedReason, `${scenario.name}: transport admission reject reason`);
        model.transportAdmissionRejectCounts[reason] =
          (model.transportAdmissionRejectCounts[reason] ?? 0) + 1;
        model.transportRejectedSettlementOrder.push(record.outboundIdentity);
        break;
      }
      case 'observeSessionOwnerAdmissionPending': {
        const record = parseBoundSessionOutboundRecord(parameters.slice(0, 9), action);
        const expectedReason = parameters[9];
        assert.equal(
          evaluateSessionOwnerAdmission(record),
          expectedReason,
          `${scenario.name}: pending admission observation`
        );
        assert.equal(expectedReason, 'targetProofPending', `${scenario.name}: unsupported pending reason`);
        model.targetProofPendingObservationCount += 1;
        break;
      }
      case 'sessionOwnerAdmitsTargetOutbound': {
        assert.equal(constraints.boundSessionCount, 1, `${scenario.name}: admission without session`);
        const record = parseBoundSessionOutboundRecord(parameters, action);
        assert.equal(
          evaluateSessionOwnerAdmission(record),
          'accepted',
          `${scenario.name}: Session owner rejected exact outbound admission`
        );
        producerPendingOutbound.delete(record.outboundIdentity);
        const admitted = {
          ...record,
          admissionSequence: lastAdmittedSequence + 1
        };
        lastAdmittedSequence = admitted.admissionSequence;
        admittedOutboundRecords.set(record.outboundIdentity, admitted);
        outboundQueueEntries.set(record.outboundIdentity, structuredClone(admitted));
        model.producerPendingOutboundCount = producerPendingOutbound.size;
        model.outboundAdmissionOrder.push(record.outboundIdentity);
        model.outboundAdmissionAcceptedCount += 1;
        if (routeAppliedTenures.has(record.actorTenureCandidate)) {
          releasedOutbound.push(record.outboundIdentity);
          model.releasedOutboundCount = releasedOutbound.length;
        } else {
          retainedOutbound.push(record.outboundIdentity);
          model.retainedOutboundCount = retainedOutbound.length;
        }
        break;
      }
      case 'rejectSessionOwnerAdmission': {
        const record = parseBoundSessionOutboundRecord(parameters.slice(0, 9), action);
        const expectedReason = parameters[9];
        const reason = evaluateSessionOwnerAdmission(record);
        assert.notEqual(reason, 'accepted', `${scenario.name}: exact admission cannot be rejected`);
        assert.equal(reason, expectedReason, `${scenario.name}: outbound admission reject reason`);
        producerPendingOutbound.delete(record.outboundIdentity);
        model.producerPendingOutboundCount = producerPendingOutbound.size;
        model.outboundAdmissionRejectCounts[reason] =
          (model.outboundAdmissionRejectCounts[reason] ?? 0) + 1;
        model.producerRejectedSettlementOrder.push(record.outboundIdentity);
        break;
      }
      case 'submitPostSealIngress': {
        requireState([
          'sealing',
          'targetCommitted',
          'targetReady',
          'authorityCompleted',
          'routeConverging',
          'routeAppliedAwaitingTerminal'
        ], action);
        assert.equal(constraints.boundSessionCount, 1, `${scenario.name}: ingress without session`);
        assert.ok(
          constraints.allowedTraffic.includes('postSealBoundSessionIngress'),
          `${scenario.name}: post-seal ingress is outside the effective milestone`
        );
        const ingressId = parameters[0];
        assert.ok(ingressId, `${scenario.name}: missing ingress id`);
        assert.ok(
          !retainedIngress.includes(ingressId) && !releasedIngress.includes(ingressId),
          `${scenario.name}: duplicate ingress id`
        );
        if (model.state === 'routeAppliedAwaitingTerminal') {
          releasedIngress.push(ingressId);
          model.releasedIngressCount = releasedIngress.length;
        } else {
          retainedIngress.push(ingressId);
          model.retainedIngressCount = retainedIngress.length;
        }
        break;
      }
      case 'routeAttempt':
        requireState(['routeConverging'], action);
        model.routeAttemptCount += 1;
        if (parameters[0] === 'transientFailure') break;
        assert.equal(parameters[0], 'apply', `${scenario.name}: unknown route result`);
        assert.ok(parameters[1], `${scenario.name}: missing route fingerprint`);
        assert.ok(
          acceptedTargetProofs.has(model.currentTenure),
          `${scenario.name}: route apply without immutable accepted target proof`
        );
        routeApplyFingerprint = parameters[1];
        model.routeApplyCount += 1;
        routeAppliedTenures.add(model.currentTenure);
        model.state = 'routeAppliedAwaitingTerminal';
        releasedOutbound.push(...retainedOutbound.splice(0));
        releasedIngress.push(...retainedIngress.splice(0));
        model.retainedOutboundCount = 0;
        model.retainedIngressCount = 0;
        model.releasedOutboundCount = releasedOutbound.length;
        model.releasedIngressCount = releasedIngress.length;
        break;
      case 'deliverReleasedOutbound':
        for (const outboundIdentity of releasedOutbound) {
          assert.equal(
            evaluateReleasedDelivery(outboundIdentity),
            'deliver',
            `${scenario.name}: released outbound is not eligible for delivery`
          );
          const admitted = admittedOutboundRecords.get(outboundIdentity);
          lastDeliveredSequence = admitted.admissionSequence;
          outboundQueueEntries.delete(outboundIdentity);
        }
        model.outboundDeliveryOrder.push(...releasedOutbound);
        model.outboundSettlementOrder.push(...releasedOutbound);
        releasedOutbound.length = 0;
        model.releasedOutboundCount = 0;
        break;
      case 'duplicateRouteApply':
        requireState(['routeAppliedAwaitingTerminal', 'stableTarget'], action);
        assert.equal(parameters[0], routeApplyFingerprint, `${scenario.name}: non-identical route duplicate`);
        model.duplicateRouteApplyCount += 1;
        break;
      case 'conflictingRouteApply':
        requireState(['routeAppliedAwaitingTerminal', 'stableTarget'], action);
        assert.notEqual(parameters[0], routeApplyFingerprint, `${scenario.name}: identical route conflict`);
        model.routeApplyConflictCount += 1;
        break;
      case 'deliverReleasedIngress':
        requireState(['stableSource', 'routeAppliedAwaitingTerminal', 'stableTarget'], action);
        model.ingressDeliveryOrder.push(...releasedIngress);
        model.ingressSettlementOrder.push(...releasedIngress);
        releasedIngress.length = 0;
        model.releasedIngressCount = 0;
        break;
      case 'rejectRouteTerminal': {
        requireState(['routeAppliedAwaitingTerminal'], action);
        assert.equal(acceptedRouteTerminalRecord, undefined, `${scenario.name}: terminal already accepted`);
        const candidate = modelRecords.routeTerminalCandidates[parameters[0]];
        assert.ok(candidate, `${scenario.name}: unknown terminal candidate`);
        assert.notDeepEqual(
          candidate,
          expectedRouteTerminalRecord(activeRelocationIdentity),
          `${scenario.name}: exact terminal cannot be rejected`
        );
        model.routeTerminalFenceRejectCount += 1;
        break;
      }
      case 'receiveRouteTerminal':
        requireState(['routeAppliedAwaitingTerminal'], action);
        assert.ok(routeApplyFingerprint, `${scenario.name}: terminal before route apply`);
        assert.equal(acceptedRouteTerminalRecord, undefined, `${scenario.name}: first terminal already accepted`);
        assert.ok(parameters[0], `${scenario.name}: missing terminal candidate`);
        assert.ok(
          modelRecords.routeTerminalCandidates[parameters[0]],
          `${scenario.name}: unknown terminal candidate`
        );
        assert.deepEqual(
          modelRecords.routeTerminalCandidates[parameters[0]],
          expectedRouteTerminalRecord(activeRelocationIdentity),
          `${scenario.name}: first route terminal does not match the exact active fence`
        );
        acceptedRouteTerminalRecord = structuredClone(
          modelRecords.routeTerminalCandidates[parameters[0]]
        );
        model.routeTerminalCount += 1;
        model.state = 'stableTarget';
        model.activeBarrierCount = 0;
        break;
      case 'duplicateRouteTerminal':
        requireState(['stableTarget'], action);
        assert.ok(acceptedRouteTerminalRecord, `${scenario.name}: no accepted terminal`);
        assert.deepEqual(
          modelRecords.routeTerminalCandidates[parameters[0]],
          acceptedRouteTerminalRecord,
          `${scenario.name}: non-identical terminal duplicate`
        );
        model.duplicateTerminalCount += 1;
        break;
      case 'conflictingRouteTerminal': {
        requireState(['stableTarget'], action);
        assert.ok(acceptedRouteTerminalRecord, `${scenario.name}: no accepted terminal`);
        const candidate = modelRecords.routeTerminalCandidates[parameters[0]];
        assert.ok(candidate, `${scenario.name}: unknown terminal conflict`);
        assert.notDeepEqual(candidate, acceptedRouteTerminalRecord, `${scenario.name}: identical conflict`);
        const withoutFingerprint = ({ terminalFingerprint, ...record }) => record;
        assert.deepEqual(
          withoutFingerprint(candidate),
          withoutFingerprint(acceptedRouteTerminalRecord),
          `${scenario.name}: conflict crosses the exact terminal control key`
        );
        model.routeTerminalConflictCount += 1;
        break;
      }
      case 'abortBeforeCommit':
        requireState(['sealing'], action);
        assert.ok(parameters[0], `${scenario.name}: missing abort fingerprint`);
        abortFingerprint = parameters[0];
        model.state = 'stableSource';
        model.activeBarrierCount = 0;
        releasedIngress.push(...retainedIngress.splice(0));
        model.retainedIngressCount = 0;
        model.releasedIngressCount = releasedIngress.length;
        break;
      case 'duplicateAbort':
        requireState(['stableSource'], action);
        assert.equal(parameters[0], abortFingerprint, `${scenario.name}: non-identical abort duplicate`);
        model.duplicateAbortCount += 1;
        break;
      case 'conflictingAbort':
        requireState(['stableSource'], action);
        assert.notEqual(parameters[0], abortFingerprint, `${scenario.name}: identical abort conflict`);
        model.abortConflictCount += 1;
        break;
      case 'requestSuccessor':
        assert.equal(
          constraints.successorRelocationAllowed,
          true,
          `${scenario.name}: successor is outside the effective milestone`
        );
        assert.equal(model.activeBarrierCount, 1, `${scenario.name}: successor without predecessor`);
        assert.ok(parameters[0], `${scenario.name}: missing successor relocation identity`);
        assert.ok(parameters[1], `${scenario.name}: missing successor owner`);
        assert.equal(
          modelRecords.relocations[parameters[0]]?.targetOwner,
          parameters[1],
          `${scenario.name}: successor differs from its immutable record`
        );
        queuedSuccessors.push({ relocationIdentity: parameters[0], targetOwner: parameters[1] });
        model.queuedSuccessorCount += 1;
        break;
      case 'observeSuccessorBlocked':
        assert.equal(model.activeBarrierCount, 1, `${scenario.name}: predecessor already terminal`);
        assert.ok(model.queuedSuccessorCount > 0, `${scenario.name}: no queued successor`);
        assert.notEqual(model.state, 'stableTarget', `${scenario.name}: successor not blocked`);
        model.successorBlockedObservationCount += 1;
        break;
      case 'admitSuccessor':
        requireState(['stableTarget'], action);
        assert.ok(model.queuedSuccessorCount > 0, `${scenario.name}: no queued successor`);
        assert.equal(model.activeBarrierCount, 0, `${scenario.name}: predecessor still active`);
        sourceTenure = model.currentTenure;
        ({ relocationIdentity: activeRelocationIdentity, targetOwner: pendingTargetOwner } =
          queuedSuccessors.shift());
        model.queuedSuccessorCount -= 1;
        model.state = 'sealing';
        model.activeBarrierCount = 1;
        routeApplyFingerprint = undefined;
        acceptedRouteTerminalRecord = undefined;
        break;
      case 'rejectMessageFollowRelay': {
        assert.ok(
          constraints.allowedTraffic.includes('boundActorRequestReply'),
          `${scenario.name}: Message Follow relay is outside the effective milestone`
        );
        const [candidateName, expectedReason] = parameters;
        const reason = evaluateMessageFollowRelay(candidateName);
        assert.notEqual(reason, 'accepted', `${scenario.name}: accepted relay cannot be rejected`);
        assert.equal(reason, expectedReason, `${scenario.name}: relay rejection reason`);
        model.messageFollowRelayRejectCounts[reason] =
          (model.messageFollowRelayRejectCounts[reason] ?? 0) + 1;
        break;
      }
      case 'followedRequest': {
        assert.ok(
          constraints.allowedTraffic.includes('boundActorRequestReply'),
          `${scenario.name}: bound Actor request/reply is outside the effective milestone`
        );
        assert.equal(
          constraints.positiveRouteCacheAllowed,
          true,
          `${scenario.name}: positive route cache is outside the effective milestone`
        );
        assert.equal(
          constraints.positiveRouteCacheMaxAgeMilliseconds,
          null,
          `${scenario.name}: positive cache is constrained to zero age`
        );
        const [candidateName, operationIdentity, replyCapability] = parameters;
        const candidate = modelRecords.messageFollowRelayCandidates[candidateName];
        assert.ok(candidate, `${scenario.name}: unknown Message Follow relay candidate`);
        assert.equal(
          evaluateMessageFollowRelay(candidateName, operationIdentity, replyCapability),
          'accepted',
          `${scenario.name}: Message Follow relay candidate rejected`
        );
        const leaseIdentity = candidate.leaseIdentity;
        model.messageFollowRelayKeys.push(
          `${leaseIdentity}|${operationIdentity}|${replyCapability}`
        );
        model.messageFollowRelayAcceptedCount += 1;
        model.followedRequestOwners.push(model.currentOwner);
        break;
      }
      case 'refreshRouteCacheToCurrentTenure':
        requireState(['stableTarget'], action);
        assert.equal(
          constraints.positiveRouteCacheAllowed,
          true,
          `${scenario.name}: positive route cache is outside the effective milestone`
        );
        routeCacheTenure = model.currentTenure;
        model.routeCacheTenure = routeCacheTenure;
        model.routeCacheRefreshCount += 1;
        break;
      case 'compareAndRemoveRouteCache': {
        requireState(['stableSource', 'stableTarget'], action);
        assert.equal(
          constraints.positiveRouteCacheAllowed,
          true,
          `${scenario.name}: route-cache compare/remove is outside the effective milestone`
        );
        const ownerBefore = model.currentOwner;
        const tenureBefore = model.currentTenure;
        const [noticeSourceTenure, expectedOutcome] = parameters;
        let outcome;
        if (routeCacheTenure === noticeSourceTenure) {
          routeCacheTenure = undefined;
          model.routeCacheRemovalCount += 1;
          outcome = 'removed';
        } else {
          model.routeCachePreserveCount += 1;
          outcome = 'preserved';
        }
        assert.equal(outcome, expectedOutcome, `${scenario.name}: cache compare/remove outcome`);
        assert.equal(model.currentOwner, ownerBefore);
        assert.equal(model.currentTenure, tenureBefore);
        model.routeCacheTenure = routeCacheTenure;
        model.routeCachePresent = routeCacheTenure !== undefined;
        break;
      }
      case 'postRoundTripRequest':
        requireState(['stableTarget'], action);
        assert.ok(
          constraints.allowedTraffic.includes('boundActorRequestReply'),
          `${scenario.name}: bound Actor request/reply is outside the effective milestone`
        );
        assert.equal(routeCacheTenure, model.currentTenure, `${scenario.name}: stale tenure resurrected`);
        model.postRoundTripRequestOwners.push(model.currentOwner);
        break;
      case 'shutdown':
        assert.ok(
          boundSessionRelocation.transitionModel.shutdownAllowedFrom.includes(model.state),
          `${scenario.name}: shutdown from ${model.state}`
        );
        model.state = 'stopped';
        model.sessionAlive = false;
        model.activeBarrierCount = 0;
        model.shutdownCount += 1;
        model.outboundFailureOrder.push(...retainedOutbound, ...releasedOutbound);
        model.ingressFailureOrder.push(...retainedIngress, ...releasedIngress);
        model.producerPendingFailureOrder.push(...producerPendingOutbound.keys());
        model.outboundSettlementOrder.push(...retainedOutbound, ...releasedOutbound);
        model.ingressSettlementOrder.push(...retainedIngress, ...releasedIngress);
        retainedOutbound.length = 0;
        releasedOutbound.length = 0;
        retainedIngress.length = 0;
        releasedIngress.length = 0;
        producerPendingOutbound.clear();
        outboundQueueEntries.clear();
        queuedSuccessors.length = 0;
        model.queuedSuccessorCount = 0;
        model.retainedOutboundCount = 0;
        model.releasedOutboundCount = 0;
        model.retainedIngressCount = 0;
        model.releasedIngressCount = 0;
        model.producerPendingOutboundCount = 0;
        break;
      case 'duplicateShutdown':
        requireState(['stopped'], action);
        model.duplicateShutdownCount += 1;
        break;
      default:
        assert.fail(`${scenario.name}: unknown model action ${action}`);
    }
    assert.ok(
      model.activeBarrierCount <= constraints.maxActiveRelocationsPerLogicalObject,
      `${scenario.name}: barrier count exceeds the effective milestone`
    );
    assert.equal(model.activeFrameCount, activeFrames.length, `${scenario.name}: ActiveFrame count`);
    assert.equal(
      model.producerPendingOutboundCount,
      producerPendingOutbound.size,
      `${scenario.name}: producer pending outbound count`
    );
    assert.equal(
      model.acceptedTargetProofCount,
      acceptedTargetProofs.size,
      `${scenario.name}: immutable accepted target proof count`
    );
    assert.equal(
      messageFollowLeases.size,
      model.messageFollowLeaseCount,
      `${scenario.name}: Message Follow lease count`
    );
    assert.deepEqual(
      [...messageFollowLeases.keys()],
      model.messageFollowLeaseIdentities,
      `${scenario.name}: Message Follow lease identities`
    );
    model.routeCacheTenure = routeCacheTenure;
    model.routeCachePresent = routeCacheTenure !== undefined;
    assert.equal(model.retainedOutboundCount, retainedOutbound.length, `${scenario.name}: outbound retain`);
    assert.equal(model.releasedOutboundCount, releasedOutbound.length, `${scenario.name}: outbound release`);
    assert.equal(
      outboundQueueEntries.size,
      retainedOutbound.length + releasedOutbound.length,
      `${scenario.name}: admitted outbound queue entry count`
    );
    for (const outboundIdentity of [...retainedOutbound, ...releasedOutbound])
      assert.deepEqual(
        outboundQueueEntries.get(outboundIdentity),
        admittedOutboundRecords.get(outboundIdentity),
        `${scenario.name}: admitted outbound queue entry`
      );
    assert.equal(model.retainedIngressCount, retainedIngress.length, `${scenario.name}: ingress retain`);
    assert.equal(model.releasedIngressCount, releasedIngress.length, `${scenario.name}: ingress release`);
  }
  assert.ok(
    Object.hasOwn(scenario.expected, 'messageFollowLeaseIdentities'),
    `${scenario.name}: exact Message Follow lease outcome is not asserted`
  );
  assert.equal(
    producerPendingOutbound.size,
    0,
    `${scenario.name}: target emitter retained an unsettled payload`
  );
  assert.deepEqual(
    model.outboundSettlementOrder,
    model.outboundAdmissionOrder,
    `${scenario.name}: SessionBindingAggregate did not settle outbound in admission order`
  );
  for (const [field, expected] of Object.entries(scenario.expected))
    assert.deepEqual(model[field], expected, `${scenario.name}: ${field}`);
  return model;
};
uniqueNames(boundSessionRelocation.modelScenarios, 'name');
const relocationModelScenarios = Object.fromEntries(
  boundSessionRelocation.modelScenarios.map(scenario => [scenario.name, scenario])
);
assert.deepEqual(Object.keys(relocationModelScenarios), [
  'M0-unbound-actor-join-completes-with-one-exact-follow-lease',
  'M1-route-apply-preserves-pre-and-post-apply-outbound-and-ingress-fifo',
  'M1-post-terminal-current-tenure-late-admission-delivers-once',
  'M1-route-converging-identical-payloads-are-distinct-admissions',
  'M1-authenticated-producer-must-match-target-candidate-before-proof',
  'M1-accepted-proof-must-match-the-pending-target-node',
  'M1-precommit-abort-restores-ingress-and-never-retains-restore-outbound',
  'M1-pre-route-apply-shutdown-settles-each-retained-owner-once',
  'M1-post-route-apply-shutdown-settles-released-fifo-once',
  'M2-successor-sealing-keeps-current-tenure-late-admission-eligible',
  'M2-admitted-predecessor-delivers-after-successor-commit',
  'M2-successor-commit-rejects-stale-predecessor-outbound-admission',
  'M2-exact-source-notice-removes-matching-route-cache-once',
  'M2-round-trip-serializes-successor-and-ignores-late-predecessor-notice'
]);
const supportedTrafficByActionVerb = {
  acceptPreSealActiveFrame: 'preSealAcceptedActiveFrame',
  submitPostSealIngress: 'postSealBoundSessionIngress',
  targetEmitterSubmitsLifecycleOutbound: 'targetPostCommitLifecycleOutboundOneWay',
  rejectMessageFollowRelay: 'boundActorRequestReply',
  followedRequest: 'boundActorRequestReply',
  postRoundTripRequest: 'boundActorRequestReply'
};
const observedTrafficByMilestone = new Map();
for (const scenario of boundSessionRelocation.modelScenarios) {
  const effective = effectiveMilestones.get(scenario.milestone).constraints;
  const observed = observedTrafficByMilestone.get(scenario.milestone) ?? new Set();
  observedTrafficByMilestone.set(scenario.milestone, observed);
  for (const action of scenario.actions) {
    const traffic = supportedTrafficByActionVerb[action.split(':')[0]];
    if (!traffic) continue;
    assert.ok(
      effective.allowedTraffic.includes(traffic),
      `${scenario.name}: ${traffic} is outside the effective milestone`
    );
    observed.add(traffic);
  }
}
assert.deepEqual(
  [...observedTrafficByMilestone.get('M1')].sort(),
  milestones.M1.allowedTraffic.toSorted(),
  'M1: supported traffic is not fully exercised'
);
const assertActionSubsequence = (scenario, actions) => {
  let cursor = 0;
  for (const action of scenario.actions)
    if (action === actions[cursor]) cursor += 1;
  assert.equal(cursor, actions.length, `${scenario.name}: missing ordered action coverage`);
};
const fifoScenario = relocationModelScenarios[
  'M1-route-apply-preserves-pre-and-post-apply-outbound-and-ingress-fifo'
];
assertActionSubsequence(fifoScenario, [
  'acceptPreSealActiveFrame:frame-1',
  'beginRelocation:relocation-1:ownerB',
  'observeSealWaitingForActiveFrames',
  'submitPostSealIngress:request-1',
  'completePreSealActiveFrame:frame-1',
  'commitTarget',
  'targetEmitterSubmitsLifecycleOutbound:push-1:node-B:19:ownerB#1:node-B:19:session-actor-1:1:1',
  'targetEmitterSubmitsLifecycleOutbound:push-2:node-B:19:ownerB#1:node-B:19:session-actor-1:1:2',
  'publishTargetReady',
  'sessionOwnerAdmitsTargetOutbound:push-1:node-B:19:ownerB#1:node-B:19:session-actor-1:1:1',
  'routeAttempt:apply:route-1',
  'sessionOwnerAdmitsTargetOutbound:push-2:node-B:19:ownerB#1:node-B:19:session-actor-1:1:2',
  'submitPostSealIngress:request-2',
  'deliverReleasedOutbound',
  'deliverReleasedIngress',
  'rejectRouteTerminal:terminal-1-wrong-binding',
  'receiveRouteTerminal:terminal-1-exact'
]);
assert.deepEqual(fifoScenario.expected.outboundDeliveryOrder, ['push-1', 'push-2']);
assert.deepEqual(fifoScenario.expected.emitterSubmissionOrder, ['push-1', 'push-2']);
assert.deepEqual(fifoScenario.expected.outboundAdmissionOrder, ['push-1', 'push-2']);
assert.deepEqual(fifoScenario.expected.ingressDeliveryOrder, ['request-1', 'request-2']);
assert.equal(fifoScenario.expected.activeFrameCount, 0);
assert.equal(fifoScenario.expected.acceptedActiveFrameCount, 1);
assert.equal(fifoScenario.expected.completedActiveFrameCount, 1);
assert.equal(fifoScenario.expected.sealWaitingObservationCount, 1);
assert.equal(fifoScenario.expected.routeTerminalFenceRejectCount, 1);
const postTerminalAdmissionScenario = relocationModelScenarios[
  'M1-post-terminal-current-tenure-late-admission-delivers-once'
];
assertActionSubsequence(postTerminalAdmissionScenario, [
  'targetEmitterSubmitsLifecycleOutbound:push-before-terminal:node-B:19:ownerB#1:node-B:19:session-actor-1:1:1',
  'targetEmitterSubmitsLifecycleOutbound:push-after-terminal:node-B:19:ownerB#1:node-B:19:session-actor-1:1:2',
  'sessionOwnerAdmitsTargetOutbound:push-before-terminal:node-B:19:ownerB#1:node-B:19:session-actor-1:1:1',
  'routeAttempt:apply:route-1',
  'receiveRouteTerminal:terminal-1-exact',
  'sessionOwnerAdmitsTargetOutbound:push-after-terminal:node-B:19:ownerB#1:node-B:19:session-actor-1:1:2',
  'deliverReleasedOutbound'
]);
assert.equal(postTerminalAdmissionScenario.expected.outboundAdmissionAcceptedCount, 2);
assert.deepEqual(postTerminalAdmissionScenario.expected.outboundDeliveryOrder, [
  'push-before-terminal',
  'push-after-terminal'
]);
const routeConvergingIdenticalPayloadScenario = relocationModelScenarios[
  'M1-route-converging-identical-payloads-are-distinct-admissions'
];
assertActionSubsequence(routeConvergingIdenticalPayloadScenario, [
  'beginRouteConvergence',
  'sessionOwnerAdmitsTargetOutbound:push-identical-1:node-B:19:ownerB#1:node-B:19:session-actor-1:1:same-payload',
  'sessionOwnerAdmitsTargetOutbound:push-identical-2:node-B:19:ownerB#1:node-B:19:session-actor-1:1:same-payload',
  'routeAttempt:apply:route-1',
  'deliverReleasedOutbound'
]);
assert.equal(routeConvergingIdenticalPayloadScenario.expected.outboundAdmissionAcceptedCount, 2);
assert.deepEqual(routeConvergingIdenticalPayloadScenario.expected.outboundDeliveryOrder, [
  'push-identical-1',
  'push-identical-2'
]);
const authenticatedProducerScenario = relocationModelScenarios[
  'M1-authenticated-producer-must-match-target-candidate-before-proof'
];
assertActionSubsequence(authenticatedProducerScenario, [
  'commitTarget',
  'rejectTargetOutboundTransport:push-forged-rid:node-X:19:ownerB#1:node-B:19:session-actor-1:1:forged-rid:authenticatedProducerNodeMismatch',
  'rejectTargetOutboundTransport:push-forged-generation:node-B:18:ownerB#1:node-B:19:session-actor-1:1:forged-generation:authenticatedProducerNodeMismatch',
  'beginRouteConvergence',
  'routeAttempt:apply:route-1'
]);
assert.deepEqual(authenticatedProducerScenario.expected.transportAdmissionRejectCounts, {
  authenticatedProducerNodeMismatch: 2
});
assert.deepEqual(
  authenticatedProducerScenario.expected.transportRejectedSettlementOrder,
  ['push-forged-rid', 'push-forged-generation']
);
const acceptedProofMismatchScenario = relocationModelScenarios[
  'M1-accepted-proof-must-match-the-pending-target-node'
];
assertActionSubsequence(acceptedProofMismatchScenario, [
  'targetEmitterSubmitsLifecycleOutbound:push-proof-mismatch:node-X:23:ownerB#1:node-X:23:session-actor-1:1:proof-mismatch',
  'beginRouteConvergence',
  'rejectSessionOwnerAdmission:push-proof-mismatch:node-X:23:ownerB#1:node-X:23:session-actor-1:1:proof-mismatch:targetProofMismatch',
  'routeAttempt:apply:route-1'
]);
assert.deepEqual(acceptedProofMismatchScenario.expected.outboundAdmissionRejectCounts, {
  targetProofMismatch: 1
});
const preApplyShutdownScenario = relocationModelScenarios[
  'M1-pre-route-apply-shutdown-settles-each-retained-owner-once'
];
assertActionSubsequence(preApplyShutdownScenario, [
  'targetEmitterSubmitsLifecycleOutbound:push-shutdown:node-B:19:ownerB#1:node-B:19:session-actor-1:1:1',
  'observeSessionOwnerAdmissionPending:push-shutdown:node-B:19:ownerB#1:node-B:19:session-actor-1:1:1:targetProofPending',
  'shutdown',
  'duplicateShutdown'
]);
assert.deepEqual(preApplyShutdownScenario.expected.producerPendingFailureOrder, ['push-shutdown']);
assert.equal(preApplyShutdownScenario.expected.targetProofPendingObservationCount, 1);
assert.equal(preApplyShutdownScenario.expected.shutdownCount, 1);
assert.equal(preApplyShutdownScenario.expected.duplicateShutdownCount, 1);
const postApplyShutdownScenario = relocationModelScenarios[
  'M1-post-route-apply-shutdown-settles-released-fifo-once'
];
assertActionSubsequence(postApplyShutdownScenario, [
  'targetEmitterSubmitsLifecycleOutbound:push-before-apply:node-B:19:ownerB#1:node-B:19:session-actor-1:1:1',
  'targetEmitterSubmitsLifecycleOutbound:push-after-apply:node-B:19:ownerB#1:node-B:19:session-actor-1:1:2',
  'publishTargetReady',
  'sessionOwnerAdmitsTargetOutbound:push-before-apply:node-B:19:ownerB#1:node-B:19:session-actor-1:1:1',
  'routeAttempt:apply:route-1',
  'sessionOwnerAdmitsTargetOutbound:push-after-apply:node-B:19:ownerB#1:node-B:19:session-actor-1:1:2',
  'submitPostSealIngress:request-after-apply',
  'duplicateRouteApply:route-1',
  'shutdown',
  'duplicateShutdown'
]);
assert.deepEqual(postApplyShutdownScenario.expected.outboundSettlementOrder, [
  'push-before-apply',
  'push-after-apply'
]);
assert.deepEqual(postApplyShutdownScenario.expected.ingressSettlementOrder, [
  'request-before-apply',
  'request-after-apply'
]);
const successorSealingAdmissionScenario = relocationModelScenarios[
  'M2-successor-sealing-keeps-current-tenure-late-admission-eligible'
];
assertActionSubsequence(successorSealingAdmissionScenario, [
  'targetEmitterSubmitsLifecycleOutbound:push-during-successor-seal:node-B:19:ownerB#1:node-B:19:session-actor-1:1:1',
  'routeAttempt:apply:route-1',
  'requestSuccessor:relocation-2:ownerA',
  'receiveRouteTerminal:terminal-1-exact',
  'admitSuccessor',
  'sessionOwnerAdmitsTargetOutbound:push-during-successor-seal:node-B:19:ownerB#1:node-B:19:session-actor-1:1:1',
  'deliverReleasedOutbound'
]);
assert.equal(successorSealingAdmissionScenario.expected.state, 'sealing');
assert.equal(successorSealingAdmissionScenario.expected.currentTenure, 'ownerB#1');
assert.equal(successorSealingAdmissionScenario.expected.outboundAdmissionAcceptedCount, 1);
assert.deepEqual(successorSealingAdmissionScenario.expected.outboundDeliveryOrder, [
  'push-during-successor-seal'
]);
const historicalAdmittedPredecessorScenario = relocationModelScenarios[
  'M2-admitted-predecessor-delivers-after-successor-commit'
];
assertActionSubsequence(historicalAdmittedPredecessorScenario, [
  'sessionOwnerAdmitsTargetOutbound:push-admitted-predecessor:node-B:19:ownerB#1:node-B:19:session-actor-1:1:1',
  'routeAttempt:apply:route-1',
  'receiveRouteTerminal:terminal-1-exact',
  'admitSuccessor',
  'commitTarget',
  'deliverReleasedOutbound'
]);
assert.equal(historicalAdmittedPredecessorScenario.expected.currentTenure, 'ownerA#2');
assert.equal(historicalAdmittedPredecessorScenario.expected.outboundAdmissionAcceptedCount, 1);
assert.deepEqual(historicalAdmittedPredecessorScenario.expected.outboundDeliveryOrder, [
  'push-admitted-predecessor'
]);
const stalePredecessorAdmissionScenario = relocationModelScenarios[
  'M2-successor-commit-rejects-stale-predecessor-outbound-admission'
];
assertActionSubsequence(stalePredecessorAdmissionScenario, [
  'routeAttempt:apply:route-1',
  'receiveRouteTerminal:terminal-1-exact',
  'admitSuccessor',
  'commitTarget',
  'targetEmitterSubmitsLifecycleOutbound:push-stale-predecessor:node-B:19:ownerB#1:node-B:19:session-actor-1:1:1',
  'rejectSessionOwnerAdmission:push-stale-predecessor:node-B:19:ownerB#1:node-B:19:session-actor-1:1:1:staleActorTenure'
]);
assert.equal(stalePredecessorAdmissionScenario.expected.currentTenure, 'ownerA#2');
assert.deepEqual(stalePredecessorAdmissionScenario.expected.outboundAdmissionRejectCounts, {
  staleActorTenure: 1
});
assert.deepEqual(stalePredecessorAdmissionScenario.expected.producerRejectedSettlementOrder, [
  'push-stale-predecessor'
]);
assert.deepEqual(stalePredecessorAdmissionScenario.expected.outboundDeliveryOrder, []);
const exactCacheRemovalScenario = relocationModelScenarios[
  'M2-exact-source-notice-removes-matching-route-cache-once'
];
assert.deepEqual(exactCacheRemovalScenario.actions, [
  'compareAndRemoveRouteCache:ownerA#0:removed'
]);
assert.equal(exactCacheRemovalScenario.expected.routeCachePresent, false);
assert.equal(exactCacheRemovalScenario.expected.routeCacheRemovalCount, 1);
assert.equal(exactCacheRemovalScenario.expected.routeCachePreserveCount, 0);
const cacheAbaScenario = relocationModelScenarios[
  'M2-round-trip-serializes-successor-and-ignores-late-predecessor-notice'
];
assertActionSubsequence(cacheAbaScenario, [
  'receiveRouteTerminal:terminal-2-exact',
  'refreshRouteCacheToCurrentTenure',
  'compareAndRemoveRouteCache:ownerA#0:preserved',
  'postRoundTripRequest:request-current-owner'
]);
assert.equal(cacheAbaScenario.expected.routeCacheTenure, 'ownerA#2');
assert.equal(cacheAbaScenario.expected.routeCacheRemovalCount, 0);
assert.equal(cacheAbaScenario.expected.routeCachePreserveCount, 1);
assertActionSubsequence(cacheAbaScenario, [
  'rejectMessageFollowRelay:relay-1-target-visited:targetAlreadyVisited',
  'rejectMessageFollowRelay:relay-1-duplicate-visited:duplicateVisitedOwner',
  'rejectMessageFollowRelay:relay-1-budget-exhausted:budgetExhausted',
  'rejectMessageFollowRelay:relay-1-expired:expired',
  'followedRequest:relay-1-exact:request-through-ownerA-cache:reply-through-ownerA-cache'
]);
assert.deepEqual(cacheAbaScenario.expected.messageFollowRelayRejectCounts, {
  targetAlreadyVisited: 1,
  duplicateVisitedOwner: 1,
  budgetExhausted: 1,
  expired: 1
});
assert.equal(cacheAbaScenario.expected.messageFollowRelayAcceptedCount, 1);
const enumerateBoundedRelocationInterleavings = (events, precedences) => {
  assert.equal(new Set(events).size, events.length, 'bounded relocation events must be unique');
  const prerequisites = new Map(events.map(event => [event, new Set()]));
  for (const [before, after] of precedences) {
    assert.ok(prerequisites.has(before), `bounded relocation: unknown predecessor ${before}`);
    assert.ok(prerequisites.has(after), `bounded relocation: unknown successor ${after}`);
    prerequisites.get(after).add(before);
  }
  const schedules = [];
  const visit = (order, used) => {
    if (order.length === events.length) {
      schedules.push([...order]);
      return;
    }
    for (const event of events) {
      if (used.has(event)) continue;
      if (![...prerequisites.get(event)].every(required => used.has(required))) continue;
      used.add(event);
      order.push(event);
      visit(order, used);
      order.pop();
      used.delete(event);
    }
  };
  visit([], new Set());
  assert.ok(schedules.length > 0, 'bounded relocation produced no legal schedule');
  assert.ok(schedules.length <= 400, 'bounded relocation schedule set exceeded its runtime cap');
  return schedules;
};
const boundedOutboundIdentity = 'bounded-push-ownerB-1';
const boundedOutboundRecord =
  `${boundedOutboundIdentity}:node-B:19:ownerB#1:node-B:19:session-actor-1:1:1`;
const boundedRelocationPrefix = [
  'beginRelocation:relocation-1:ownerB',
  'commitTarget',
  `targetEmitterSubmitsLifecycleOutbound:${boundedOutboundRecord}`,
  'publishTargetReady',
  'completeAuthority',
  'beginRouteConvergence'
];
const boundedRelocationPrefixWithoutOutbound = [
  'beginRelocation:relocation-1:ownerB',
  'commitTarget',
  'publishTargetReady',
  'completeAuthority',
  'beginRouteConvergence'
];
const boundedRelocationActionByEvent = {
  routeApply: 'routeAttempt:apply:route-1',
  requestSuccessor: 'requestSuccessor:relocation-2:ownerA',
  terminal: 'receiveRouteTerminal:terminal-1-exact',
  admitSuccessor: 'admitSuccessor',
  successorCommit: 'commitTarget',
  lateSubmit: `targetEmitterSubmitsLifecycleOutbound:${boundedOutboundRecord}`,
  admission: `sessionOwnerAdmitsTargetOutbound:${boundedOutboundRecord}`,
  delivery: 'deliverReleasedOutbound',
  staleAdmission: `rejectSessionOwnerAdmission:${boundedOutboundRecord}:staleActorTenure`
};
const acceptedBoundedEvents = [
  'routeApply',
  'requestSuccessor',
  'terminal',
  'admitSuccessor',
  'successorCommit',
  'admission',
  'delivery'
];
const acceptedBoundedPrecedences = [
  ['routeApply', 'terminal'],
  ['requestSuccessor', 'terminal'],
  ['requestSuccessor', 'admitSuccessor'],
  ['terminal', 'admitSuccessor'],
  ['admitSuccessor', 'successorCommit'],
  ['admission', 'successorCommit'],
  ['admission', 'delivery'],
  ['routeApply', 'delivery']
];
const acceptedWithoutDuplicateSchedules = enumerateBoundedRelocationInterleavings(
  acceptedBoundedEvents,
  acceptedBoundedPrecedences
);
const staleFirstAdmissionSchedules = enumerateBoundedRelocationInterleavings(
  [
    'routeApply',
    'requestSuccessor',
    'terminal',
    'admitSuccessor',
    'successorCommit',
    'lateSubmit',
    'staleAdmission'
  ],
  [
    ['routeApply', 'terminal'],
    ['requestSuccessor', 'terminal'],
    ['requestSuccessor', 'admitSuccessor'],
    ['terminal', 'admitSuccessor'],
    ['admitSuccessor', 'successorCommit'],
    ['successorCommit', 'lateSubmit'],
    ['lateSubmit', 'staleAdmission']
  ]
);
assert.equal(acceptedWithoutDuplicateSchedules.length, 36);
assert.equal(staleFirstAdmissionSchedules.length, 2);
let boundedScheduleCount = 0;
let terminalBeforeAcceptedAdmissionScheduleCount = 0;
let historicalAdmittedDeliveryScheduleCount = 0;
for (const [index, schedule] of acceptedWithoutDuplicateSchedules.entries()) {
    const deliveryAfterSuccessorCommit =
      schedule.indexOf('delivery') > schedule.indexOf('successorCommit');
    if (schedule.indexOf('admission') > schedule.indexOf('terminal'))
      terminalBeforeAcceptedAdmissionScheduleCount += 1;
    if (deliveryAfterSuccessorCommit) historicalAdmittedDeliveryScheduleCount += 1;
    const model = runRelocationModel({
      name: `bounded-M2-accepted-${index}`,
      milestone: 'M2',
      profile: 'actorJoin',
      actions: [
        ...boundedRelocationPrefix,
        ...schedule.map(event => boundedRelocationActionByEvent[event])
      ],
      expected: {
        state: 'targetCommitted',
        currentOwner: 'ownerA',
        currentTenure: 'ownerA#2',
        activeBarrierCount: 1,
        queuedSuccessorCount: 0,
        producerPendingOutboundCount: 0,
        emitterSubmissionOrder: [boundedOutboundIdentity],
        outboundAdmissionOrder: [boundedOutboundIdentity],
        outboundAdmissionAcceptedCount: 1,
        pendingCurrentActorTenureValidationCount: 1,
        admittedActorTenureReauthorizationCount: 0,
        retainedOutboundCount: 0,
        releasedOutboundCount: 0,
        outboundDeliveryOrder: [boundedOutboundIdentity],
        outboundSettlementOrder: [boundedOutboundIdentity],
        routeApplyCount: 1,
        routeTerminalCount: 1,
        messageFollowLeaseIdentities: ['follow-lease-1', 'follow-lease-2']
      }
    });
    assert.deepEqual(
      model.outboundDeliveryOrder,
      model.outboundAdmissionOrder,
      `accepted-${index}: admitted FIFO delivery`
    );
    boundedScheduleCount += 1;
}
for (const [index, schedule] of staleFirstAdmissionSchedules.entries()) {
  runRelocationModel({
    name: `bounded-M2-stale-first-admission-${index}`,
    milestone: 'M2',
    profile: 'actorJoin',
    actions: [
      ...boundedRelocationPrefixWithoutOutbound,
      ...schedule.map(event => boundedRelocationActionByEvent[event])
    ],
    expected: {
      state: 'targetCommitted',
      currentOwner: 'ownerA',
      currentTenure: 'ownerA#2',
      activeBarrierCount: 1,
      queuedSuccessorCount: 0,
      producerPendingOutboundCount: 0,
      emitterSubmissionOrder: [boundedOutboundIdentity],
      outboundAdmissionOrder: [],
      outboundAdmissionAcceptedCount: 0,
      pendingCurrentActorTenureValidationCount: 1,
      admittedActorTenureReauthorizationCount: 0,
      outboundAdmissionRejectCounts: { staleActorTenure: 1 },
      producerRejectedSettlementOrder: [boundedOutboundIdentity],
      retainedOutboundCount: 0,
      releasedOutboundCount: 0,
      outboundDeliveryOrder: [],
      outboundSettlementOrder: [],
      routeApplyCount: 1,
      routeTerminalCount: 1,
      messageFollowLeaseIdentities: ['follow-lease-1', 'follow-lease-2']
    }
  });
  boundedScheduleCount += 1;
}
assert.equal(terminalBeforeAcceptedAdmissionScheduleCount, 10);
assert.equal(historicalAdmittedDeliveryScheduleCount, 10);
assert.equal(boundedScheduleCount, 38);
for (const scenario of boundSessionRelocation.modelScenarios)
  runRelocationModel(scenario);
for (const milestone of ['M0', 'M1', 'M2'])
  assert.ok(
    boundSessionRelocation.modelScenarios.some(scenario => scenario.milestone === milestone),
    `${milestone}: no executable model scenario`
  );
assert.deepEqual(boundSessionRelocation.applicationBoundary, {
  restoreBeforeOwnershipCommit: {
    boundSessionSendResult: 'unavailable',
    physicalDeliveryCount: 0,
    retainedPayloadCount: 0
  },
  postCommitLifecycleBeforeRouteApply: {
    targetEmitterSubmitResult: 'producerPendingUntilAdmission',
    boundSessionOneWayAdmission: 'retained',
    physicalDeliveryCount: 0,
    actorTurnWaitsForPhysicalDelivery: false,
    retentionOwner: 'sessionBindingAggregate'
  },
  afterAtomicRouteApply: {
    delayedSessionOwnerAdmissionJoinsReleasedFifo: true,
    retainedReleaseCount: 1,
    eventualPhysicalDeliveryCount: 1,
    routeTerminalRequiredForRelease: false,
    duplicateRouteApplyAdditionalReleaseCount: 0
  },
  afterExactRouteTerminal: {
    successorAdmissionAllowed: true,
    currentTenureDelayedOutboundAdmission: 'appendToReleasedFifo',
    stalePredecessorOutboundAdmission: 'rejectAndSettleWithoutDelivery',
    admittedPredecessorAfterSuccessorCommit:
      'deliverFromSessionBindingAggregateWithoutActorTenureReauthorization',
    duplicateTerminalAdditionalDeliveryCount: 0
  }
});
const requiredHardFences = {
  wireIngress: ['authenticatedPeer', 'targetNodeGeneration', 'actorId', 'objectGeneration'],
  routeCommit: [
    'actorTenure',
    'sessionIdentity',
    'bindingGeneration',
    'sealIdentity',
    'acceptedHighWater'
  ],
  routeTerminal: [
    'relocationIdentity',
    'actorTenure',
    'sessionIdentity',
    'bindingGeneration',
    'sealIdentity',
    'acceptedHighWater',
    'actionAndResult',
    'terminalFingerprint'
  ],
  boundSessionPacket: boundSessionPacketFenceFields,
  messageFollowRelay: [
    'exactSourceTenure',
    'exactTargetTenure',
    'originalOperationIdentity',
    'originalReplyCapability',
    'visitedOwners',
    'remainingBudget',
    'expiry'
  ],
  replyTerminal: [
    'originalOperationIdentity',
    'originalReplyCapability',
    'sessionIdentity',
    'bindingGeneration'
  ]
};
assert.deepEqual(Object.keys(boundSessionRelocation.hardFenceBoundaries), Object.keys(requiredHardFences));
for (const [boundary, requiredFields] of Object.entries(requiredHardFences)) {
  assert.equal(
    new Set(boundSessionRelocation.hardFenceBoundaries[boundary]).size,
    boundSessionRelocation.hardFenceBoundaries[boundary].length,
    `${boundary}: duplicate hard fence`
  );
  for (const field of requiredFields)
    assert.ok(boundSessionRelocation.hardFenceBoundaries[boundary].includes(field), `${boundary}: ${field}`);
}
assert.deepEqual(boundSessionRelocation.trafficScenarios, {
  actorJoinLifecycle: 'target-lifecycle-callback-bound-session-push',
  genericTarget: 'target-bound-session-push-before-route-convergence'
});
for (const scenario of Object.values(boundSessionRelocation.trafficScenarios))
  assert.ok(relocationTraffic[scenario]);
assert.ok(relocationTerminals[boundSessionRelocation.terminalScenario]);
assert.deepEqual(boundSessionRelocation.invariants, {
  trafficSubmittedWhileSealedIsRetained: true,
  deliveryBeforeRouteApply: 0,
  routeApplyReleasesRetainedTrafficExactlyOnce: true,
  routeTerminalGatesSuccessorAdmission: true,
  duplicateRouteTerminalAdditionalDelivery: 0,
  sameKeyDifferentTerminalFingerprintMutatesState: false,
  successorRelocationFence: [
    'relocationId',
    'bindingGeneration',
    'sessionIdentity'
  ],
  latePredecessorTerminalAffectsSuccessor: false,
  publicJoinCompletionWaitsForRouteTerminal: false,
  sameCallResubmitCount: 0
});

assert.equal(relocationAdapters.fixture, 'zlink.framework.relocation-conformance-adapters');
assert.equal(relocationAdapters.version, 1);
assert.equal(
  relocationAdapters.behaviorFixture,
  'framework/runtime/conformance/relocation-behavior-v1.json'
);
assert.equal(
  relocationAdapters.boundSessionFixture,
  'framework/runtime/conformance/bound-session-relocation-v1.json'
);
assert.deepEqual(relocationAdapters.requiredLanguages, requiredRelocationLanguages);
assert.deepEqual(relocationAdapters.requiredBehaviorGroups, requiredRelocationGroups);
assert.equal(relocationAdapters.focusedCommandTimeoutSeconds, 600);
assert.deepEqual(relocationAdapters.evidenceContract, {
  testIdentifierMatch: 'exactLiteralSubstringInSource',
  directFixtureConsumerRequiredFields: ['path', 'fixtureReference'],
  directFixtureReferenceMatch: 'exactLiteralSubstringInSource'
});
assert.deepEqual(
  relocationAdapters.adapters.map(({ language }) => language),
  requiredRelocationLanguages
);
const allowedAdapterConnections = new Set(['direct', 'mapped', 'pending']);
const allowedAdapterCoverage = new Set(['covered', 'partial', 'pending', 'e2eRequired']);
for (const adapter of relocationAdapters.adapters) {
  assert.ok(allowedAdapterConnections.has(adapter.fixtureConnection), adapter.language);
  assert.deepEqual(Object.keys(adapter.coverage), requiredRelocationGroups, adapter.language);
  for (const coverage of Object.values(adapter.coverage))
    assert.ok(allowedAdapterCoverage.has(coverage), `${adapter.language}: ${coverage}`);
  assert.ok(Array.isArray(adapter.evidence) && adapter.evidence.length > 0, adapter.language);
  const evidenceGroups = new Set();
  for (const evidence of adapter.evidence) {
    const evidencePath = resolve(repositoryRoot, evidence.path);
    await access(evidencePath);
    const evidenceSource = await readFile(evidencePath, 'utf8');
    assert.ok(Array.isArray(evidence.behaviorGroups) && evidence.behaviorGroups.length > 0);
    for (const group of evidence.behaviorGroups) {
      assert.ok(requiredRelocationGroups.includes(group), `${evidence.path}: ${group}`);
      evidenceGroups.add(group);
    }
    assert.ok(Array.isArray(evidence.tests) && evidence.tests.length > 0, evidence.path);
    for (const identifier of evidence.tests) {
      assert.ok(identifier.length > 0, evidence.path);
      assert.ok(
        evidenceSource.includes(identifier),
        `${evidence.path}: missing exact test identifier ${identifier}`
      );
    }
  }
  for (const [group, coverage] of Object.entries(adapter.coverage)) {
    if (coverage === 'covered' || coverage === 'partial')
      assert.ok(evidenceGroups.has(group), `${adapter.language}: ${group} lacks evidence`);
  }
  if (adapter.fixtureConnection === 'direct') {
    assert.deepEqual(
      Object.keys(adapter.fixtureConsumer),
      relocationAdapters.evidenceContract.directFixtureConsumerRequiredFields
    );
    assert.equal(
      adapter.fixtureConsumer.fixtureReference,
      'relocation-behavior-v1.json'
    );
    const consumerPath = resolve(repositoryRoot, adapter.fixtureConsumer.path);
    await access(consumerPath);
    const consumerSource = await readFile(consumerPath, 'utf8');
    assert.ok(
      consumerSource.includes(adapter.fixtureConsumer.fixtureReference),
      `${adapter.language}: direct consumer does not reference the behavior fixture`
    );
  }
  assert.ok(Array.isArray(adapter.focusedCommands) && adapter.focusedCommands.length > 0);
  const isFocusedTestCommand = ([command, ...commandArgs]) =>
    command === 'ctest'
      || (command === 'dotnet' && commandArgs[0] === 'test')
      || (command.endsWith('gradlew')
        && commandArgs.some(argument => argument === 'test' || argument.endsWith(':test')))
      || (command === 'node' && commandArgs.includes('--test'));
  const focusedTestCommands = adapter.focusedCommands.filter(isFocusedTestCommand);
  assert.ok(focusedTestCommands.length > 0, `${adapter.language}: no focused test command`);
  for (const evidence of adapter.evidence) {
    const fileName = evidence.path.split('/').at(-1);
    const sourceIdentifier = fileName.replace(/\.[^.]+$/, '');
    assert.ok(
      focusedTestCommands.some(command => {
        const commandText = command.join('\n');
        return commandText.includes(sourceIdentifier)
          || evidence.tests.some(identifier => commandText.includes(identifier));
      }),
      `${adapter.language}: focused test commands do not select ${evidence.path}`
    );
  }
  for (const command of adapter.focusedCommands) {
    const commandText = command.join(' ');
    assert.equal(
      /e2e|sample/i.test(commandText),
      false,
      `${adapter.language}: focused command includes a sample or E2E path: ${commandText}`
    );
  }
}

assert.equal(relocationE2e.fixture, 'zlink.framework.relocation-e2e-scenarios');
assert.equal(relocationE2e.version, 1);
assert.equal(relocationE2e.executionPolicy, 'inventoryOnly');
assert.equal(relocationE2e.runFromFocusedConformance, false);
assert.deepEqual(relocationE2e.requiredLanguages, requiredRelocationLanguages);
uniqueNames(relocationE2e.scenarios, 'name');
const relocationE2eScenarioNames = [
  'actual-redis-aggregate-cas-visibility',
  'public-spot-profile-lifecycle-and-traffic-order',
  'source-process-death-before-ownership-commit',
  'target-process-death-after-ownership-commit',
  'physical-stream-continuity-during-bound-actor-relocation',
  'real-old-node-message-follow',
  'lost-session-route-ack-retransmission',
  'restart-without-durable-seal-evidence',
  'relocation-unit-interruption-one-second-goal'
];
assert.deepEqual(
  relocationE2e.scenarios.map(({ name }) => name),
  relocationE2eScenarioNames
);
for (const scenario of relocationE2e.scenarios) {
  assert.ok(scenario.reason.length > 0, scenario.name);
  for (const profile of scenario.requiredProfiles)
    assert.ok(profiles[profile], `${scenario.name}: ${profile}`);
}
const publicSpotProfile = relocationE2e.scenarios.find(
  ({ name }) => name === 'public-spot-profile-lifecycle-and-traffic-order'
);
assert.deepEqual(publicSpotProfile.requiredProfiles, [
  'spotWideUserSpot',
  'perActorUserSpot',
  'instanceSpot'
]);
assert.deepEqual(publicSpotProfile.expectation, {
  savedTemporaryDirectOrder: true,
  sourceClosingReason: 'RelocationOut',
  sourceClosingCount: 1,
  targetMembershipCallbackCount: 0,
  sourceMembershipCallbackCount: 0,
  relocationTerminalCount: 1
});
const sourceDeath = relocationE2e.scenarios.find(
  ({ name }) => name === 'source-process-death-before-ownership-commit'
);
const targetDeath = relocationE2e.scenarios.find(
  ({ name }) => name === 'target-process-death-after-ownership-commit'
);
const hostRelocationProfiles = [
  'actorHostHandoff',
  'perActorUserSpot',
  'spotWideUserSpot',
  'instanceSpot'
];
assert.deepEqual(sourceDeath.requiredProfiles, hostRelocationProfiles);
assert.deepEqual(targetDeath.requiredProfiles, hostRelocationProfiles);
const durableSealRestart = relocationE2e.scenarios.find(
  ({ name }) => name === 'restart-without-durable-seal-evidence'
);
assert.deepEqual(durableSealRestart.requiredProfiles, [
  'actorJoin',
  'actorHostHandoff',
  'perActorUserSpot',
  'spotWideUserSpot'
]);
const interruptionGoal = relocationE2e.scenarios.find(
  ({ name }) => name === 'relocation-unit-interruption-one-second-goal'
);
assert.deepEqual(interruptionGoal.requiredProfiles, [
  'actorJoin',
  'actorHostHandoff',
  'perActorUserSpot',
  'spotWideUserSpot',
  'instanceSpot'
]);
assert.deepEqual(interruptionGoal.expectation, {
  measurementStart: 'sourceAdmissionSealed',
  measurementEnd: 'targetReadyPublished',
  goalMilliseconds: 1000,
  goalExceededIsFailure: false,
  goalExceededTriggersRollback: false,
  goalExceededTriggersRetry: false
});
assert.deepEqual(
  relocationE2e.languageScenarioMatrix.allowedStatuses,
  ['notRun', 'pass', 'fail', 'deferred']
);
assert.deepEqual(
  relocationE2e.languageScenarioMatrix.evidenceRequiredForStatuses,
  ['pass', 'fail', 'deferred']
);
assert.deepEqual(
  relocationE2e.languageScenarioMatrix.rows.map(({ language }) => language),
  requiredRelocationLanguages
);
for (const row of relocationE2e.languageScenarioMatrix.rows) {
  assert.deepEqual(Object.keys(row.scenarios), relocationE2eScenarioNames, row.language);
  for (const [scenarioName, cell] of Object.entries(row.scenarios)) {
    assert.ok(
      relocationE2e.languageScenarioMatrix.allowedStatuses.includes(cell.status),
      `${row.language}: ${scenarioName}: ${cell.status}`
    );
    assert.ok(Array.isArray(cell.evidence), `${row.language}: ${scenarioName}`);
    if (relocationE2e.languageScenarioMatrix.evidenceRequiredForStatuses
      .includes(cell.status))
      assert.ok(cell.evidence.length > 0, `${row.language}: ${scenarioName}: evidence`);
    else
      assert.deepEqual(cell, { status: 'notRun', evidence: [] });
  }
}

console.log('runtime conformance fixtures: PASS');
