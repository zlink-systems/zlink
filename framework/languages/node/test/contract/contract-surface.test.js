const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const test = require('node:test');

const workspaceRoot = path.resolve(__dirname, '..', '..');
const nodeDocRoot = path.resolve(workspaceRoot, '..', '..', 'doc', 'framework', 'node');
const interfaceSpecRoot = path.join(
  nodeDocRoot,
  '..',
  'common',
  'spec',
  'server',
  'languages',
  'node',
  'interfaces'
);
const declarationsRoot = path.join(workspaceRoot, 'packages', 'framework', 'dist', 'contracts');
const internalLocationCodecHelpers = new Set([
  // §10 keeps these declarations only as a labeled, non-normative snapshot.
  'IZLinkLocationRuntimeQuery',
  'IZLinkLocationStore',
  'IZLinkPeerLocationResolver',
  'ZLinkSessionPacketDispatcher',
  'tryParseZLinkLocationAutoConnectType',
  'tryParseZLinkLocationRole',
  'zlinkLocationAutoConnectTypeName',
  'zlinkLocationRoleName'
]);
const publicContractSnapshot = JSON.parse(fs.readFileSync(
  path.join(__dirname, 'fixtures', 'node-public-contract.json'),
  'utf8'
));

test('server package declarations collectively cover the exact interface catalog exports', () => {
  const spec = readTree(interfaceSpecRoot);
  const declarations = [
    readTree(declarationsRoot),
    readTree(path.join(workspaceRoot, 'packages', 'nestjs', 'dist')),
    readTree(path.join(workspaceRoot, 'packages', 'framework-locations-redis', 'dist'))
  ].join('\n');
  const missing = [];

  for (const name of exportedCatalogNames(frameworkCatalog(spec)).filter((name) => !internalLocationCodecHelpers.has(name))) {
    const declarationPattern = new RegExp(`\\b(?:interface|type|enum|function)\\s+${name}\\b`);
    if (!declarationPattern.test(declarations)) {
      missing.push(name);
    }
  }

  assert.deepEqual(missing.sort(), []);
});

test('framework runtime exports decorator factories and enums from the catalog', () => {
  const framework = require('../../packages/framework/dist');
  const spec = readTree(interfaceSpecRoot);
  const missing = [];

  for (const name of runtimeCatalogNames(frameworkCatalog(spec)).filter((name) => !internalLocationCodecHelpers.has(name))) {
    if (!(name in framework)) {
      missing.push(name);
    }
  }

  assert.deepEqual(missing.sort(), []);
});

test('framework public root does not expose direct runtime start hosts', () => {
  const framework = require('../../packages/framework/dist');
  const hiddenNames = [
    'ZLinkFrameworkRuntimeHost',
    'ZLinkRegistryRuntime',
    'ZLinkStreamBindingRuntime'
  ];

  const exposed = hiddenNames.filter((name) => name in framework);

  assert.deepEqual(exposed, []);
});

test('Nest options builder matches the exact public member set', () => {
  const nestjs = require('../../packages/nestjs/dist');
  const builder = nestjs.zlinkFramework();
  const expected = [...publicContractSnapshot.nestjsFrameworkBuilder];
  const methods = new Set();
  let prototype = builder;
  while (prototype && prototype !== Object.prototype) {
    for (const name of Object.getOwnPropertyNames(prototype)) {
      if (name !== 'constructor' && typeof builder[name] === 'function') methods.add(name);
    }
    prototype = Object.getPrototypeOf(prototype);
  }
  for (const name of expected) {
    assert.equal(methods.has(name), true, `Nest builder runtime member missing: ${name}`);
  }
  assert.deepEqual(
    [...methods].sort(),
    expected.sort(),
    'Nest builder runtime member set must not expose internal helpers'
  );
  assert.equal('channelName' in builder, false);

  const inbound = builder.configureInboundDispatch();
  assert.equal(typeof inbound.applicationHwmBytes, 'function');
  assert.equal(typeof inbound.applicationHwmProfile, 'function');
  assert.equal(typeof inbound.processMemoryLimitBytes, 'function');
});

test('Nest inbound dispatch builder keeps fluent HWM changes in build output', () => {
  const nestjs = require('../../packages/nestjs/dist');
  const builder = nestjs.zlinkFramework();

  builder.configureInboundDispatch()
    .applicationHwmBytes(8192n)
    .processMemoryLimitBytes(16384n);

  const built = builder.build();
  assert.equal(built.inboundDispatch.applicationHwmBytes, 8192n);
  assert.equal(built.inboundDispatch.processMemoryLimitBytes, 16384n);
});

test('Nest RouteMesh builder keeps the formal scheduler limits in build output', () => {
  const nestjs = require('../../packages/nestjs/dist');
  const builder = nestjs.zlinkFramework();

  builder.addRouteMesh('api')
    .setActorLimit(11)
    .setSpotLimit(22)
    .setActivationConcurrency(33);

  const built = builder.build();
  assert.equal(built.spotNodes.api.actorLimit, 11);
  assert.equal(built.spotNodes.api.spotLimit, 22);
  assert.equal(built.spotNodes.api.activationConcurrencyLimit, 33);
});

test('Node public contract snapshot pins the binding package version', () => {
  const binding = require('../../node_modules/@zlink-systems/zlink/package.json');
  assert.equal(binding.version, publicContractSnapshot.bindingVersion);
});

test('stream connector public root does not expose raw frame or header codecs', () => {
  const connector = require('../../packages/stream-connector/dist');
  const exposed = ['ZlinkStreamFrameCodec', 'ZlinkStreamHeaderCodec']
    .filter((name) => name in connector);

  assert.deepEqual(exposed, []);
});

test('worker options expose the formal scheduler limits', () => {
  const declarations = readTree(declarationsRoot);
  const workerOptions = declarationBody(declarations, 'ZLinkWorkerOptions');

  assert.equal(workerOptions.includes('maxThreads'), true);
  assert.equal(workerOptions.includes('maxQueueLength'), true);
  assert.equal(workerOptions.includes('minThreads'), true);
  assert.equal(workerOptions.includes('idleTimeoutMs'), true);
});

test('location and relocation stores have separate public registration surfaces', () => {
  const declarations = readTree(declarationsRoot);
  const frameworkOptions = declarationBody(declarations, 'ZLinkFrameworkOptions');
  const relocationStore = declarationBody(declarations, 'ZLinkRelocationStore');

  assert.equal(frameworkOptions.includes('addLocationStore'), true);
  assert.equal(frameworkOptions.includes('addRelocationStore'), true);
  assert.equal(relocationStore.includes('put('), true);
  assert.equal(relocationStore.includes('read('), true);
  assert.equal(relocationStore.includes('renew('), true);
  assert.equal(relocationStore.includes('delete('), true);
  assert.equal(relocationStore.includes('putRelocation'), false);
  assert.equal(relocationStore.includes('getRelocation'), false);
  assert.equal(frameworkOptions.includes('addRedis'), false);
  assert.equal(frameworkOptions.includes('addStores'), false);
});

test('channel request yield and Mesh channel roles match the exact contract', () => {
  const declarations = readTree(declarationsRoot);
  const requestCall = declarationBody(declarations, 'ZLinkRequestCall');
  const channelRequestCall = declarationBody(declarations, 'ZLinkChannelRequestCall');
  const meshNodeBuilder = declarationBody(declarations, 'ZLinkMeshNodeBuilder');
  const meshChannelBuilder = declarationBody(declarations, 'ZLinkMeshChannelBuilder');
  const meshChannelClient = declarationBody(declarations, 'ZLinkMeshChannelClientBuilder');
  const meshChannelServer = declarationBody(declarations, 'ZLinkMeshChannelServerBuilder');

  assert.equal(requestCall.includes('yield'), false);
  assert.equal(interfaceExtends(channelRequestCall, 'ZLinkRequestCall'), true);
  assert.match(channelRequestCall, /yield<TReply>\(signal\?: AbortSignal\): Promise<TReply>/);
  assert.match(meshNodeBuilder, /channel\(channelName: string\): ZLinkMeshChannelBuilder/);
  assert.equal(meshNodeBuilder.includes('channelName('), false);
  assert.match(meshChannelBuilder, /client\(\): ZLinkMeshChannelClientBuilder/);
  assert.match(meshChannelBuilder, /server\(\): ZLinkMeshChannelServerBuilder/);
  assert.equal(meshChannelClient.includes('setWeight'), false);
  assert.match(meshChannelServer, /setWeight\(weight: number\): this/);
  assert.match(meshChannelServer, /addHandlerGroup\(groupName: string\): this/);
});

test('runtime topology and supporting exact names are declared by their production packages', () => {
  const frameworkDeclarations = readTree(declarationsRoot);
  const nestDeclarations = readTree(path.join(workspaceRoot, 'packages', 'nestjs', 'dist'));
  const redisDeclarations = readTree(path.join(workspaceRoot, 'packages', 'framework-locations-redis', 'dist'));
  const exactInterfaceCatalog = readTree(interfaceSpecRoot);
  const runtimeTypes = [
    'ZLinkFrameworkRelocationOptions',
    'ZLinkFrameworkRelocationResult',
    'ZLinkFrameworkTerminationResult',
    'ZLinkFrameworkLifecycleOptions',
    'ZLinkFrameworkRuntimeStatus',
    'ZLinkObservationLoss',
    'ZLinkObservedStatus',
    'ZLinkFrameworkRuntime',
    'ZLinkPeerStatus',
    'ZLinkChannelStatus',
    'ZLinkPlacementStatus',
    'ZLinkRouteMeshStatus',
    'ZLinkRouteMeshRuntime',
    'ZLinkClientServerTargetStatus',
    'ZLinkClientServerStatus',
    'ZLinkClientServerRuntime',
    'ZLinkFanoutStatus',
    'ZLinkFanoutRuntime',
    'ZLinkReservedObjectCreation'
  ];

  for (const name of runtimeTypes) {
    assert.match(frameworkDeclarations, new RegExp(`\\binterface ${name}\\b`));
  }
  const runtimeAliases = [
    'ZLinkClientServerRole',
    'ZLinkMessageFlowReason'
  ];
  for (const name of runtimeAliases) {
    assert.match(frameworkDeclarations, new RegExp(`\\btype ${name}\\b`));
  }
  const exactRouteMeshNames = [
    'ZLinkFrameworkRelocationMode',
    'ZLinkFrameworkRelocationOutcome',
    'ZLinkFrameworkRelocationReason',
    'ZLinkFrameworkRelocationOptions',
    'ZLinkFrameworkRelocationResult',
    'ZLinkFrameworkTerminationOutcome',
    'ZLinkFrameworkTerminationReason',
    'ZLinkFrameworkTerminationResult',
    'ZLinkFrameworkLifecycleOptions',
    'ZLinkFrameworkRuntimeStatus',
    'ZLinkObservationLoss',
    'ZLinkObservedStatus',
    'ZLinkFrameworkRuntime',
    'ZLinkTopologyState',
    'ZLinkPeerState',
    'ZLinkTopologyReason',
    'ZLinkPeerStatus',
    'ZLinkChannelStatus',
    'ZLinkPlacementStatus',
    'ZLinkRouteMeshStatus',
    'ZLinkRouteMeshRuntime'
  ];
  for (const name of exactRouteMeshNames) {
    assert.match(exactInterfaceCatalog, new RegExp(`\\b(?:interface|type|enum) ${name}\\b`));
  }
  const routeMeshRuntime = declarationBody(frameworkDeclarations, 'ZLinkRouteMeshRuntime');
  const frameworkRuntime = declarationBody(frameworkDeclarations, 'ZLinkFrameworkRuntime');
  assert.match(frameworkRuntime, /readonly status: ZLinkFrameworkRuntimeStatus/);
  assert.match(frameworkRuntime, /relocate\(options: ZLinkFrameworkRelocationOptions\): Promise<ZLinkFrameworkRelocationResult>/);
  assert.match(frameworkRuntime, /shutdown\(options\?: ZLinkFrameworkLifecycleOptions\): Promise<ZLinkFrameworkTerminationResult>/);
  assert.match(routeMeshRuntime, /snapshot\(meshName: string\): ZLinkRouteMeshStatus/);
  assert.match(routeMeshRuntime, /observe\(meshName: string, capacity\?: number, signal\?: AbortSignal\): AsyncIterable<ZLinkObservedStatus<ZLinkRouteMeshStatus>>/);
  assert.match(routeMeshRuntime, /isReady\(meshName: string\): boolean/);
  assert.doesNotMatch(routeMeshRuntime, /\bdrain\(/);
  assert.doesNotMatch(routeMeshRuntime, /\bawaitDrained\(/);
  for (const removed of ['ZLinkMeshDrainSnapshot', 'ZLinkDrainForceReason', 'ZLinkMeshDrainResult']) {
    assert.doesNotMatch(frameworkDeclarations, new RegExp(`\\b(?:interface|type) ${removed}\\b`));
    assert.doesNotMatch(exactInterfaceCatalog, new RegExp(`\\b(?:interface|type) ${removed}\\b`));
  }
  for (const removed of [
    'ZLinkMeshRuntimeEvent',
    'ZLinkMeshClaimSnapshot',
    'ZLinkClientServerRuntimeEvent',
    'ZLinkFanoutRuntimeEvent'
  ]) {
    assert.doesNotMatch(frameworkDeclarations, new RegExp(`\\b(?:interface|type) ${removed}\\b`));
  }
  assert.match(nestDeclarations, /\binterface ZLinkNestMeshChannelClientBuilder\b/);
  assert.match(nestDeclarations, /\binterface ZLinkNestMeshChannelServerBuilder\b/);
  assert.match(redisDeclarations, /\bclass ZLinkRedisRelocationStore\b/);
  assert.match(redisDeclarations, /\binterface ZLinkRedisRelocationOptions\b/);
});

test('Redis Location and relocation capabilities remain separate public classes', () => {
  const declarations = readTree(path.join(workspaceRoot, 'packages', 'framework-locations-redis', 'dist'));
  const locationHeader = declarations.match(/export declare class ZLinkRedisLocationStore implements[\s\S]*? \{/);
  const relocationHeader = declarations.match(/export declare class ZLinkRedisRelocationStore implements[^\{]+ \{/);

  assert.ok(locationHeader);
  assert.ok(relocationHeader);
  assert.equal(locationHeader[0].includes('ZLinkRelocationStore'), false);
  assert.equal(relocationHeader[0].includes('ZLinkRelocationStore'), true);
});

test('diagnostics options do not expose inert native diagnostics configuration', () => {
  const declarations = readTree(declarationsRoot);
  const diagnosticsOptions = declarationBody(declarations, 'ZLinkDiagnosticsOptions');

  assert.equal(diagnosticsOptions.includes('includeNativeDiagnostics'), false);
});

test('public declarations do not expose raw runtime events or monitoring registration', () => {
  const contracts = readTree(
    path.join(workspaceRoot, 'packages', 'framework', 'src', 'contracts')
  );
  const spec = readTree(interfaceSpecRoot);

  for (const removed of [
    'ZLinkMonitoringOptions',
    'ZLinkPollingMonitoringRegistration',
    'ZLinkRuntimeEventHandler',
    'ZLinkSocketEvent',
    'ZLinkLocationRuntimeEvent',
    'ZLinkSpotEvent',
    'ZLinkSpotPeerKind',
    'ZLinkSpotPeerSource',
    'ZLinkSpotPeerState',
    'ZLinkStreamDiagnostic'
  ]) {
    assert.doesNotMatch(contracts, new RegExp(`\\b${removed}\\b`));
    assert.doesNotMatch(spec, new RegExp(`\\b${removed}\\b`));
  }
  assert.doesNotMatch(contracts, /\bmonitoring\??:/);
  assert.doesNotMatch(spec, /\bzlinkRuntimeEventHandler\b/);
  assert.doesNotMatch(spec, /\bnativeCode\??:/);
});

test('framework configuration surface does not expose codec callback options', () => {
  const text = [
    readTree(declarationsRoot),
    fs.readFileSync(path.join(workspaceRoot, 'packages', 'framework', 'src', 'contracts', 'Configuration', 'Builders.ts'), 'utf8'),
    fs.readFileSync(path.join(workspaceRoot, 'packages', 'framework', 'src', 'contracts', 'Configuration', 'Registration.ts'), 'utf8'),
    fs.readFileSync(path.join(workspaceRoot, 'packages', 'nestjs', 'src', 'index.ts'), 'utf8')
  ].join('\n');
  const forbidden = [
    [/codecs\s*\(\s*configure/, 'codecs(configure) builder callback'],
    [/readonly\s+codecs\?:\s*\([^=]/, 'registration codecs callback property'],
    [/codecs\s*:\s*\([^=]/, 'module codecs callback property']
  ];
  const offenders = [];

  for (const [pattern, reason] of forbidden) {
    if (pattern.test(text)) {
      offenders.push(reason);
    }
  }

  assert.deepEqual(offenders.sort(), []);
});

test('NestJS module options expose only builder-created opaque configuration', () => {
  const declarations = readTree(path.join(workspaceRoot, 'packages', 'nestjs', 'dist'));
  const moduleOptions = declarationBody(declarations, 'ZLinkModuleOptions');
  const forbidden = [
    'clientServerChannels',
    'fanoutChannels',
    'routerMeshes',
    'spotNodes',
    'streams',
    'channels',
    'routeChannels',
    'streamNodes'
  ];
  const exposed = forbidden.filter((name) => moduleOptions.includes(name));

  assert.deepEqual(exposed, []);
});

test('NestJS public declarations do not export object-shaped module option types', () => {
  const declarations = readTree(path.join(workspaceRoot, 'packages', 'nestjs', 'dist'));
  const forbiddenExports = [
    'ZLinkNestClientServerChannelOptions',
    'ZLinkNestFanoutChannelOptions',
    'ZLinkNestRouterMeshOptions'
  ];
  const exposed = forbiddenExports.filter((name) =>
    new RegExp(`\\bexport\\s+interface\\s+${name}\\b`).test(declarations)
  );

  assert.deepEqual(exposed, []);
});

test('framework package exports only the public root contract', () => {
  const packageJson = JSON.parse(
    fs.readFileSync(path.join(workspaceRoot, 'packages', 'framework', 'package.json'), 'utf8'));

  assert.deepEqual(Object.keys(packageJson.exports).sort(), ['.']);
  assert.equal(packageJson.exports['.'].default, './dist/index.js');
  assert.equal(packageJson.exports['.'].types, './dist/index.d.ts');
});

test('framework public root excludes raw route storage and serializer selection details', () => {
  const publicRoot = fs.readFileSync(path.join(declarationsRoot, 'index.d.ts'), 'utf8');
  const codecDeclarations = readTree(path.join(declarationsRoot, 'Codecs'));
  assert.doesNotMatch(publicRoot, /ZLinkRouteKind|ZLinkRouteLocation/);
  assert.doesNotMatch(
    codecDeclarations,
    /ZLinkSerializerSelectionContext|canSerialize|selection|packetName|messageType|parseMessage/
  );
});

test('NestJS package declarations stay inside declared public package boundaries', () => {
  const packageJson = JSON.parse(
    fs.readFileSync(path.join(workspaceRoot, 'packages', 'nestjs', 'package.json'), 'utf8'));
  const declarations = readTree(path.join(workspaceRoot, 'packages', 'nestjs', 'dist'));

  assert.deepEqual(Object.keys(packageJson.exports).sort(), ['.']);
  assert.deepEqual(packageJson.files, ['dist']);
  assert.equal(packageJson.exports['.'].default, './dist/index.js');
  assert.equal(packageJson.exports['.'].types, './dist/index.d.ts');
  assert.doesNotMatch(declarations, /@zlink-systems\/framework\/nest-integration/);
});

test('spot actor lifecycle handler registration API is not public', () => {
  const declarations = readTree(declarationsRoot);
  const workspaceText = [
    declarations,
    readTree(path.join(workspaceRoot, 'samples')),
    readTree(nodeDocRoot)
  ].join('\n');
  const removedNames = [
    'addActorJoin',
    'addPostActorJoined',
    'addActorLeft',
    'SpotActorJoinHandler',
    'PostActorJoinedHandler',
    'ActorLeftHandler',
    'ZLinkSpotActorJoinHandler',
    'ZLinkSpotPostActorJoinedHandler',
    'ZLinkSpotActorLeftHandler'
  ];

  const remaining = removedNames.filter((name) => workspaceText.includes(name));

  assert.deepEqual(remaining, []);
});

test('entry spot public surface separates creation and membership from user spot admission', () => {
  const declarations = readTree(declarationsRoot);
  const entrySpot = declarationBody(declarations, 'ZLinkEntrySpot');
  const membershipLifecycle = declarationBody(declarations, 'ZLinkSpotActorMembershipLifecycle');
  const userLifecycle = declarationBody(declarations, 'ZLinkUserSpotActorLifecycle');
  const entryContext = declarationBody(declarations, 'ZLinkEntrySpotContext');

  assert.equal(interfaceExtends(entrySpot, 'ZLinkSpot'), false);
  assert.equal(interfaceExtends(entrySpot, 'ZLinkSpotActorMembershipLifecycle'), true);
  assert.equal(entrySpot.includes('onCreate?'), false);
  assert.equal(entrySpot.includes('onActorJoin'), false);
  assert.equal(membershipLifecycle.includes('onActorJoin'), false);
  assert.equal(userLifecycle.includes('onActorJoin'), true);
  assert.equal(entrySpot.includes('onCreateActor'), true);
  assert.equal(entrySpot.includes('onActorRelocated'), false);
  assert.equal(membershipLifecycle.includes('onJoinedActor'), true);
  assert.equal(membershipLifecycle.includes('onLeaveActor'), true);
  assert.equal(entryContext.includes('leaveActor'), false);
  assert.equal(entryContext.includes('destroyActor'), true);
  assert.equal(declarationBody(declarations, 'ZLinkSpotContext').includes('destroyActor'), false);
  assert.equal(entryContext.includes('close('), false);
});

test('actor join and one-way calls expose only their target terminators', () => {
  const actorContracts = fs.readFileSync(
    path.join(workspaceRoot, 'packages', 'framework', 'src', 'contracts', 'Actors', 'ZLinkActorFactory.ts'),
    'utf8');
  const boundSessionContracts = fs.readFileSync(
    path.join(workspaceRoot, 'packages', 'framework', 'src', 'contracts', 'Streams', 'BoundSessionContracts.ts'),
    'utf8');

  const actorJoinCall = declarationBody(actorContracts, 'ZLinkActorJoinCall');
  const actorJoinSpotCall = declarationBody(actorContracts, 'ZLinkActorJoinSpotCall');
  const actorJoinEntrySpotCall = declarationBody(actorContracts, 'ZLinkActorJoinEntrySpotCall');

  assert.match(actorJoinCall, /defer\(\): void/);
  assert.equal(actorJoinCall.includes('submit'), false);
  assert.equal(actorJoinCall.includes('yield'), false);
  assert.equal(actorContracts.includes('ZLinkActorYieldJoinCall'), false);
  assert.equal(interfaceExtends(actorJoinSpotCall, 'ZLinkActorJoinCall'), true);
  assert.equal(interfaceExtends(actorJoinEntrySpotCall, 'ZLinkActorJoinCall'), true);
  const boundSessionSendCall = declarationBody(boundSessionContracts, 'ZLinkBoundSessionSendCall');
  assert.match(boundSessionSendCall, /submit\(signal\?: AbortSignal\): Promise<void>/);
  assert.equal(boundSessionSendCall.includes('yield('), false);
});

test('spot manager exposes exact single-use stable-type calls and generation-fenced refs', () => {
  const declarations = readTree(declarationsRoot);
  const spotManager = declarationBody(declarations, 'ZLinkSpotManager');
  const meshNodeBuilder = declarationBody(declarations, 'ZLinkMeshNodeBuilder');
  const objectServerBuilder = declarationBody(declarations, 'ZLinkMeshObjectServerBuilder');

  assert.match(spotManager, /create\(spotType: string\): ZLinkSpotCreateCall/);
  assert.match(spotManager, /getOrCreate\(spotId: SpotId, spotType: string\): ZLinkSpotGetOrCreateCall/);
  assert.match(spotManager, /find\(spotId: SpotId, signal\?: AbortSignal\): Promise<SpotRef \| undefined>/);
  assert.match(spotManager, /close\(spot: SpotRef, signal\?: AbortSignal\): Promise<boolean>/);
  assert.doesNotMatch(spotManager, /Type<TSpot>|meshName: string,\s*spotType/);
  assert.match(objectServerBuilder, /addEntrySpot<TEntrySpot extends ZLinkEntrySpot>/);
  assert.doesNotMatch(meshNodeBuilder, /configureEntrySpot/);
  assert.doesNotMatch(declarations, /ZLinkEntrySpotOptions/);
});

test('location wire enums retain values while provider write enums stay internal', () => {
  const framework = require('../../packages/framework/dist');
  const internal = require('../../packages/framework/dist/internal');
  const expectedLocationEnums = {
    ZLinkLocationRole: {
      Invalid: 0,
      Spot: 2,
      Router: 3,
      Dealer: 4,
      Pub: 5,
      Sub: 6
    },
    ZLinkLocationWriteIntent: {
      NewClaim: 1,
      Renew: 2,
      Takeover: 3
    },
    ZLinkLocationWriteStatus: {
      Stored: 'stored',
      IgnoredStale: 'ignoredStale',
      RejectedConflict: 'rejectedConflict'
    },
    ZLinkLocationTopologyState: {
      Discovered: 1,
      Connecting: 2,
      Ready: 3,
      Lost: 4,
      Error: 5,
      Stopped: 6
    },
    ZLinkSpotKind: {
      Invalid: 'invalid',
      Entry: 'entry',
      User: 'user',
      Instance: 'instance'
    }
  };

  // pickEnumValues projects the actual enum onto the expected key set, so a member
  // ADDED to the runtime enum was invisible to deepEqual. Only AutoConnectType had a
  // reverse key-set check; the other eight were unguarded. Assert both directions for
  // every enum instead, so an added member fails rather than going unverified.
  for (const [enumName, expected] of Object.entries(expectedLocationEnums)) {
    const enumObject = enumName === 'ZLinkLocationWriteIntent'
      || enumName === 'ZLinkLocationWriteStatus'
      ? internal[enumName]
      : framework[enumName];
    assert.deepEqual(pickEnumValues(enumObject, Object.keys(expected)), expected, enumName);
    assert.deepEqual(
      Object.keys(enumObject)
        .filter((name) => Number.isNaN(Number(name)))
        .sort(),
      Object.keys(expected).sort(),
      `${enumName} key set`
    );
  }

  assert.equal(framework.ZLinkLocationAutoConnectType, undefined);
  assert.equal(framework.ZLinkLocationKind, undefined);
  assert.equal(framework.ZLinkRouteKind, undefined);
  assert.equal(framework.ZLinkLocationChangeType, undefined);
  assert.equal(framework.ZLinkLocationWriteIntent, undefined);
  assert.equal(framework.ZLinkLocationWriteStatus, undefined);

  assert.equal(framework.zlinkLocationAutoConnectTypeName, undefined);
  assert.equal(framework.zlinkLocationRoleName, undefined);
});

test('formal declarations expose role-specific ClientServer builders and exclude removed combined builders', () => {
  const frameworkDeclarations = readTree(declarationsRoot);
  const nestDeclarations = readTree(path.join(workspaceRoot, 'packages', 'nestjs', 'dist'));
  const removedNames = [
    'ZLinkClientServerChannelBuilder',
    'ZLinkRouteMeshChannelBuilder',
    'ZLinkNestClientServerChannelBuilder'
  ];

  for (const name of removedNames) {
    assert.equal(frameworkDeclarations.includes(name), false, `framework declaration retained ${name}`);
    assert.equal(nestDeclarations.includes(name), false, `Nest declaration retained ${name}`);
  }

  for (const name of [
    'ZLinkClientServerChannelRoleBuilder',
    'ZLinkClientServerChannelClientBuilder',
    'ZLinkClientServerChannelServerBuilder',
    'addClientServerChannel'
  ]) {
    assert.equal(frameworkDeclarations.includes(name), true, `framework declaration missing ${name}`);
  }
  for (const name of [
    'ZLinkNestClientServerChannelRoleBuilder',
    'ZLinkNestClientServerChannelClientBuilder',
    'ZLinkNestClientServerChannelServerBuilder',
    'addClientServerChannel'
  ]) {
    assert.equal(nestDeclarations.includes(name), true, `Nest declaration missing ${name}`);
  }
});

test('Nest declarations expose the common inbound dispatch builder contract', () => {
  const declarations = readTree(path.join(workspaceRoot, 'packages', 'nestjs', 'dist'));
  const builder = declarationBody(declarations, 'ZLinkNestFrameworkOptionsBuilder');
  assert.match(builder, /configureInboundDispatch\(\): ZLinkInboundDispatchOptions/);
});

test('framework error kind values and exception surface match the shared table', () => {
  const framework = require('../../packages/framework/dist');
  const expected = [
    ['NotFound', 0],
    ['AlreadyExists', 1],
    ['TypeMismatch', 2],
    ['NotConfigured', 3],
    ['Rejected', 4],
    ['Unavailable', 5],
    ['CapacityExceeded', 6],
    ['DeadlineExceeded', 7],
    ['ShuttingDown', 8],
    ['ProtocolError', 9],
    ['InvalidOperation', 10],
    ['DataLost', 11],
    ['InternalFailure', 12]
  ];

  assert.equal(expected.length, 13);
  const enumNames = Object.keys(framework.ZLinkFrameworkErrorKind)
    .filter((name) => Number.isNaN(Number(name)));
  assert.equal(enumNames.length, 13);
  assert.deepEqual(
    enumNames.sort(),
    expected.map(([name]) => name).sort()
  );
  for (const [name, numericValue] of expected) {
    const kind = framework.ZLinkFrameworkErrorKind[name];
    assert.equal(kind, numericValue, name);
  }
  const error = new framework.ZLinkFrameworkException(
    framework.ZLinkFrameworkErrorKind.Unavailable,
    'route is currently unavailable'
  );
  assert.equal(error.kind, framework.ZLinkFrameworkErrorKind.Unavailable);
  assert.equal('code' in error, false);
  assert.equal('isRetriable' in error, false);
  assert.equal(framework.ZLINK_FRAMEWORK_ERROR_KIND_VALUES, undefined);
  assert.equal(framework.isZLinkFrameworkErrorRetriableByDefault, undefined);
});

test('handler filter public contract exposes only the five supported dispatch kinds', () => {
  const declarations = readTree(declarationsRoot);
  const filter = declarationBody(declarations, 'ZLinkHandlerFilter');
  const context = declarationBody(declarations, 'ZLinkHandlerFilterContext');

  assert.match(filter, /context: ZLinkHandlerFilterContext/);
  assert.match(filter, /next: ZLinkHandlerFilterNext/);
  assert.match(filter, /\): Promise<void>/);
  assert.match(context, /dispatchKind: ZLinkHandlerDispatchKind/);
  assert.match(
    declarations,
    /type ZLinkHandlerFilterNext\s*=\s*\(\) => Promise<void>/
  );
  assert.deepEqual(
    Object.values(require('../../packages/framework/dist').ZLinkHandlerDispatchKind),
    [
      'nodeDirectSend',
      'nodeDirectRequest',
      'channelSend',
      'channelRequest',
      'classicFanout'
    ]
  );
  assert.doesNotMatch(filter + context, /ownerKind|descriptor|endpoint|socket/i);
});

test('location contract exposes only opaque provider primitives and aggregate operational queries', () => {
  const declarations = readTree(declarationsRoot);
  const locationStore = declarationBody(declarations, 'ZLinkLocationStore');
  const runtimeQuery = declarationBody(declarations, 'ZLinkLocationRuntimeQuery');
  const topologyFilter = declarationBody(declarations, 'ZLinkLocationTopologyFilter');
  const topologyEntry = declarationBody(declarations, 'ZLinkLocationTopologyEntry');
  const serviceSummaryFilter = declarationBody(declarations, 'ZLinkLocationServiceSummaryFilter');
  const clientServerDescriptor = declarationBody(declarations, 'ZLinkClientServerServerDescriptor');

  assert.doesNotMatch(interfaceHeader(declarations, 'ZLinkLocationStore'), /extends/);
  for (const operation of ['read', 'write', 'scan']) {
    assert.match(locationStore, new RegExp(`\\b${operation}\\(`));
  }
  assert.match(locationStore, /\bdispose\?\(\): void \| Promise<void>/);
  for (const removed of [
    'updateMeshNode', 'listClientServers', 'readAuthority', 'claimOwnerLease',
    'reserveRelocationCapacity', 'prepareAggregate', 'removeAllByOwner',
    'getMeshNodeChangeStamp'
  ]) assert.doesNotMatch(locationStore, new RegExp(`\\b${removed}\\(`));
  assert.match(
    declarations,
    /kind: 'restore';[\s\S]*?payload: Uint8Array;[\s\S]*?expectedOwner: ZLinkLocationOwnerToken/
  );
  assert.match(runtimeQuery, /listMeshNodeDescriptors\(/);
  assert.match(runtimeQuery, /listTopology\(/);
  assert.match(runtimeQuery, /listServiceSummaries\(/);
  assert.match(runtimeQuery, /Promise<ZLinkLocationPage<ZLinkLocationServiceSummary>>/);
  assert.doesNotMatch(runtimeQuery, /list(?:Peer|Spot|Actor|Route)Locations\(/);
  assert.match(topologyFilter, /readonly meshName\?: string/);
  assert.match(topologyFilter, /readonly nodeRid\?: RoutingId/);
  assert.match(topologyFilter, /readonly state\?: ZLinkLocationTopologyState/);
  assert.doesNotMatch(topologyFilter, /kind|role|spotId|actorId/);
  for (const field of ['meshName', 'nodeRid', 'endpoint', 'draining', 'state', 'updatedAt']) {
    assert.match(topologyEntry, new RegExp(`readonly ${field}:`));
  }
  assert.doesNotMatch(topologyEntry, /kind|role|spotId|actorId|desiredCount|readyCount|errorCode/);
  assert.match(serviceSummaryFilter, /readonly meshName\?: string/);
  assert.doesNotMatch(serviceSummaryFilter, /role|kind/);

  for (const removed of [
    'ZLinkClientServerLocationStore', 'ZLinkMeshNodeLocationStore',
    'ZLinkSpotLocationStore', 'ZLinkActorLocationStore',
    'ZLinkRoutingIdSlotAllocationStore'
  ]) assert.equal(declarations.includes(`interface ${removed}`), false, removed);
  for (const field of [
    'channelName', 'serverRid', 'lifecycleGeneration', 'descriptorRevision',
    'endpoint', 'weight', 'state', 'securityIdentity', 'ownerId',
    'leaseGeneration', 'updatedAt'
  ]) {
    assert.match(clientServerDescriptor, new RegExp(`readonly ${field}:`));
  }

});

test('actor declarations resolve global ActorId calls and expose fluent manager shapes', () => {
  const declarations = readTree(declarationsRoot);
  const actorClient = declarationBody(declarations, 'ZLinkActorClient');
  const actorSendCall = declarationBody(declarations, 'ZLinkActorSendCall');
  const actorRequestCall = declarationBody(declarations, 'ZLinkActorRequestCall');
  const manager = declarationBody(declarations, 'ZLinkActorManager');
  const createCall = declarationBody(declarations, 'ZLinkActorCreateCall');
  const getOrCreateCall = declarationBody(declarations, 'ZLinkActorGetOrCreateCall');
  const sessionActors = declarationBody(declarations, 'ZLinkSessionActors');
  const actorRef = declarationBody(declarations, 'ActorRef');

  assert.match(actorClient, /sendToActor\(actorId: ActorId, message: unknown\): ZLinkActorSendCall/);
  assert.match(actorClient, /requestToActor\(actorId: ActorId, request: unknown\): ZLinkActorRequestCall/);
  assert.equal(actorClient.includes('meshName:'), false);
  assert.equal(actorClient.includes('actor: ActorRef'), false);
  assert.match(actorSendCall, /metadata\(key: string, value: string\): this/);
  assert.match(actorSendCall, /submit\(signal\?: AbortSignal\): Promise<void>/);
  assert.equal(actorSendCall.includes('packetName('), false);
  assert.match(actorRequestCall, /metadata\(key: string, value: string\): this/);
  assert.equal(actorRequestCall.includes('packetName('), false);
  assert.match(actorRequestCall, /timeout\(timeoutMs: number\): this/);
  assert.match(actorRequestCall, /submit<TReply>\(signal\?: AbortSignal\): Promise<TReply>/);
  assert.match(actorRequestCall, /yield<TReply>\(signal\?: AbortSignal\): Promise<TReply>/);
  assert.match(manager, /create\(actorId: ActorId, actorType: string\): ZLinkActorCreateCall/);
  assert.match(manager, /getOrCreate\(actorId: ActorId, actorType: string\): ZLinkActorGetOrCreateCall/);
  assert.match(manager, /find\(actorId: ActorId, signal\?: AbortSignal\): Promise<ActorRef \| undefined>/);
  assert.match(manager, /findSpot\(actorId: ActorId, signal\?: AbortSignal\): Promise<SpotRef \| undefined>/);
  assert.match(manager, /destroy\(actor: ActorRef, signal\?: AbortSignal\): Promise<boolean>/);
  for (const call of [createCall, getOrCreateCall]) {
    assert.match(call, /inMesh\(meshName: string\): this/);
    assert.match(call, /request\(request: unknown\): this/);
    assert.match(call, /timeout\(timeoutMs: number\): this/);
    assert.match(call, /submit\(signal\?: AbortSignal\): Promise<ZLinkActorCreateResult>/);
    assert.match(call, /yield\(signal\?: AbortSignal\): Promise<ZLinkActorCreateResult>/);
    assert.equal(call.includes('preferredNodeRid'), false);
  }
  assert.match(actorRef, /readonly nodeRid: RoutingId/);
  assert.match(actorRef, /readonly actorId: ActorId/);
  assert.match(actorRef, /readonly objectGeneration: bigint/);
  assert.match(actorRef, /readonly meshName: string/);
  assert.doesNotMatch(declarations, /\bZLinkActorRefSnapshot\b/);
  assert.match(sessionActors, /bindOrGet\(actor: ActorRef, signal\?: AbortSignal\): Promise<ZLinkSessionActor>/);
});

test('one-way call declarations complete without exposing transport admission results', () => {
  const declarations = readTree(declarationsRoot);
  const sendCall = declarationBody(declarations, 'ZLinkSendCall');
  const fanoutPublishCall = declarationBody(declarations, 'ZLinkFanoutPublishCall');
  const publishCall = declarationBody(declarations, 'ZLinkPublishCall');
  const actorSendCall = declarationBody(declarations, 'ZLinkActorSendCall');
  const boundSessionSendCall = declarationBody(declarations, 'ZLinkBoundSessionSendCall');
  const sessionSendCall = declarationBody(declarations, 'ZLinkSessionSendCall');
  const sessionReplyCall = declarationBody(declarations, 'ZLinkSessionReplyCall');
  const sessionActor = declarationBody(declarations, 'ZLinkSessionActor');

  assert.match(sendCall, /submit\(signal\?: AbortSignal\): Promise<void>/);
  assert.match(fanoutPublishCall, /submit\(signal\?: AbortSignal\): Promise<void>/);
  assert.match(publishCall, /submit\(signal\?: AbortSignal\): Promise<void>/);
  assert.match(boundSessionSendCall, /submit\(signal\?: AbortSignal\): Promise<void>/);
  assert.match(sessionSendCall, /submit\(signal\?: AbortSignal\): Promise<void>/);
  assert.match(sessionReplyCall, /submit\(signal\?: AbortSignal\): Promise<void>/);
  assert.match(sessionActor, /relay\(payload: ZLinkMessage, signal\?: AbortSignal\): Promise<void>/);
  assert.doesNotMatch(declarations, /ZLinkSubmitResult|ZLinkPublishResult|ZLinkSubmitStatus|ZLinkLogicalMulticastDetail/);
  for (const call of [sendCall, fanoutPublishCall, publishCall, actorSendCall,
    boundSessionSendCall, sessionSendCall, sessionReplyCall]) {
    assert.doesNotMatch(call, new RegExp(['try', 'Submit'].join('')));
  }
});

test('stream connector and server HTTP one-way calls expose Promise<void>', () => {
  const streamCalls = fs.readFileSync(
    path.join(workspaceRoot, 'packages', 'stream-connector', 'dist', 'Contracts', 'Calls', 'ZlinkStreamCalls.d.ts'),
    'utf8'
  );
  const serverHttp = fs.readFileSync(
    path.join(workspaceRoot, 'packages', 'nestjs', 'dist', 'http-client-module.d.ts'),
    'utf8'
  );

  assert.match(
    declarationBody(streamCalls, 'ZlinkStreamSendCall'),
    /submit\(\): Promise<void>/
  );
  assert.match(
    declarationBody(serverHttp, 'ZLinkServerHttpRequestBuilder'),
    /submit\(\): Promise<void>/
  );
});

test('route client surface scopes node routing by MeshName and resolves channels globally', () => {
  const declarations = readTree(declarationsRoot);
  const routeClient = declarationBody(declarations, 'ZLinkRouteClient');

  assert.match(routeClient, /sendToNode\(meshName: string, targetNodeRid: RoutingId, message: unknown\): ZLinkSendCall/);
  assert.match(routeClient, /requestToNode\(meshName: string, targetNodeRid: RoutingId, request: unknown\): ZLinkRequestCall/);
  assert.match(routeClient, /sendToChannel\(channelName: string, message: unknown\): ZLinkSendCall/);
  assert.match(routeClient, /requestToChannel\(channelName: string, request: unknown\): ZLinkChannelRequestCall/);
  assert.doesNotMatch(routeClient, /SpotHandle|sendToSpot|requestToSpot/);
});

test('Spot public declarations use SpotId calls and keep Instance handlers actor-free', () => {
  const declarations = readTree(declarationsRoot);
  const spotOutbound = declarationBody(declarations, 'ZLinkSpotOutbound');
  const instanceContext = declarationBody(declarations, 'ZLinkInstanceSpotContext');
  const instanceHandlers = declarationBody(declarations, 'ZLinkInstanceSpotHandlerRegistry');
  const spotsIndex = fs.readFileSync(path.join(declarationsRoot, 'Spots', 'index.d.ts'), 'utf8');

  assert.match(spotOutbound, /sendToSpot\(spotId: SpotId, message: unknown\): ZLinkSpotSendCall/);
  assert.match(spotOutbound, /requestToSpot\(spotId: SpotId, request: unknown\): ZLinkSpotRequestCall/);
  assert.match(instanceContext, /readonly handlers: ZLinkInstanceSpotHandlerRegistry/);
  assert.match(instanceHandlers, /addPacket<THandler>\(handlerType: Type<THandler>\): this/);
  assert.doesNotMatch(instanceHandlers, /addSubscribe|addActor/);
  assert.doesNotMatch(spotsIndex, /SpotHandle/);
});

test('old public contract names from the redesign rename table do not re-enter node surfaces', () => {
  const forbidden = [
    'IZLinkSpotLocationResolver',
    'ZLinkSpotLocationResolver',
    'IZLinkActorLocationResolver',
    'ZLinkActorLocationResolver',
    'IZLinkRouteLocationResolver',
    'ZLinkRouteLocationResolver',
    'IZLinkActorRefResolver',
    'ZLinkActorRefResolver',
    'ZLinkResolveFreshness',
    'resolveFreshness',
    'ZLinkLocationCanonicalNames',
    'ZLinkSpotLocationRidResolver',
    'SpotLocationRidResolver',
    'PositiveCache',
    'positiveCache',
    'ActorIdConflict',
    'actorIdConflict',
    'locationKey'
  ];
  const allowlist = new Set([
    'test/contract/contract-surface.test.js'
  ]);
  const matches = [];

  for (const file of readTextFiles([
    path.join(workspaceRoot, 'packages', 'framework'),
    path.join(workspaceRoot, 'test'),
    path.join(workspaceRoot, 'e2e'),
    path.join(workspaceRoot, 'samples')
  ])) {
    const relativePath = path.relative(workspaceRoot, file);
    if (allowlist.has(relativePath)) {
      continue;
    }
    const text = fs.readFileSync(file, 'utf8');
    for (const name of forbidden) {
      if (text.includes(name)) {
        matches.push(`${relativePath}: ${name}`);
      }
    }
  }

  assert.deepEqual(matches.sort(), []);
});

function exportedCatalogNames(spec) {
  return uniqueMatches(spec, /^export\s+(?:interface|type|enum|function)\s+([A-Za-z][A-Za-z0-9_]*)/gm);
}

function frameworkCatalog(spec) {
  return spec.split(/^### \d+\.\d+ @zlink-systems\/nestjs:/m)[0];
}

function runtimeCatalogNames(spec) {
  return uniqueMatches(spec, /^export\s+(?:enum|function)\s+([A-Za-z][A-Za-z0-9_]*)/gm);
}

function uniqueMatches(text, pattern) {
  return [...new Set([...text.matchAll(pattern)].map((match) => match[1]))].sort();
}

function declarationBody(text, name) {
  const match = text.match(new RegExp(`export interface ${name}(?:<[^>{]+>)?(?: [^{]+)? \\{([\\s\\S]*?)\\n\\}`));
  assert.ok(match, `missing declaration for ${name}`);
  return match[0];
}

function interfaceHeader(text, name) {
  const match = text.match(new RegExp(`export interface ${name}(?:<[^>{]+>)?(?: [^{]+)? \\{`));
  assert.ok(match, `missing declaration header for ${name}`);
  return match[0];
}

function interfaceExtends(declaration, baseName) {
  const header = declaration.slice(0, declaration.indexOf('{'));
  return new RegExp(`\\bextends\\b[^{]*\\b${baseName}\\b`).test(header);
}

function pickEnumValues(enumObject, names) {
  assert.ok(enumObject, 'missing enum object');
  return Object.fromEntries(names.map((name) => [name, enumObject[name]]));
}

test('framework public root excludes internal registration implementation', () => {
  const framework = require('../../packages/framework/dist');
  const declarations = fs.readFileSync(path.join(declarationsRoot, '..', 'index.d.ts'), 'utf8') +
    fs.readFileSync(path.join(declarationsRoot, 'index.d.ts'), 'utf8') +
    fs.readFileSync(path.join(declarationsRoot, 'Configuration', 'index.d.ts'), 'utf8');
  const forbidden = [
    'createFrameworkRegistration',
    'createFrameworkOptions',
    'ZLinkFrameworkRegistration',
    'ZLinkFrameworkRegistrationOptions',
    'ZLinkSpotNodeRegistrationOptions',
    'ZLinkProviderResolver'
  ];

  for (const name of forbidden) {
    assert.equal(Object.hasOwn(framework, name), false, `${name} must not be a runtime export`);
    assert.equal(new RegExp(`export (?:declare )?(?:function|interface|type|class) ${name}\\b`).test(declarations), false,
      `${name} must not be a declaration export`);
  }
});

function readTree(root) {
  let text = '';
  for (const entry of fs.readdirSync(root, { withFileTypes: true })) {
    const fullPath = path.join(root, entry.name);
    if (entry.isDirectory()) {
      text += readTree(fullPath);
      continue;
    }
    if (/\.(?:ts|js|md)$/.test(entry.name)) {
      text += fs.readFileSync(fullPath, 'utf8');
    }
  }
  return text;
}

function readTextFiles(roots) {
  const files = [];
  for (const root of roots) {
    collectTextFiles(root, files);
  }
  return files;
}

function collectTextFiles(root, files) {
  for (const entry of fs.readdirSync(root, { withFileTypes: true })) {
    const fullPath = path.join(root, entry.name);
    if (entry.name === 'node_modules' || entry.name === 'dist' || entry.name === '.git') {
      continue;
    }
    if (entry.isDirectory()) {
      collectTextFiles(fullPath, files);
      continue;
    }
    if (/\.(?:ts|tsx|js|mjs|cjs|md|json|sh|ps1)$/.test(entry.name)) {
      files.push(fullPath);
    }
  }
}
