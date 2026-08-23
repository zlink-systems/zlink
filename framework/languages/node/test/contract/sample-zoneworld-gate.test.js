const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const test = require('node:test');

const nodeRoot = path.resolve(__dirname, '../..');

function read(relativePath) {
  return fs.readFileSync(path.join(nodeRoot, relativePath), 'utf8');
}

test('ZoneWorld Node sample contains the language server and headless scenario client', () => {
  assert.ok(fs.existsSync(path.join(nodeRoot, 'samples/ZoneWorld/package.json')));
  assert.ok(fs.existsSync(path.join(nodeRoot, 'samples/ZoneWorld/Server/Gateway/main.ts')));
  assert.ok(fs.existsSync(path.join(nodeRoot, 'samples/ZoneWorld/Server/ZoneNode/main.ts')));
  assert.ok(fs.existsSync(path.join(nodeRoot, 'samples/ZoneWorld/Server/Ops/main.ts')));
  assert.ok(fs.existsSync(path.join(nodeRoot, 'samples/ZoneWorld/Client/main.ts')));
  assert.ok(fs.existsSync(path.join(nodeRoot, 'samples/ZoneWorld/run_sample.ps1')));
  assert.match(read('samples/run_samples.sh'), /ZoneWorld/);
  assert.match(read('samples/run_samples.ps1'), /ZoneWorld/);
});

test('ZoneWorld roles use one physical MeshNode with automatic logical handlers', () => {
  const zoneNode = read('samples/ZoneWorld/Server/ZoneNode/zone-node-module.ts');
  const gateway = read('samples/ZoneWorld/Server/Gateway/gateway-module.ts');
  const ops = read('samples/ZoneWorld/Server/Ops/ops-module.ts');
  const nodeHandlers = read('samples/ZoneWorld/Server/ZoneNode/Infrastructure/ZLink/Handlers/node-channel-handlers.ts');
  const opsHandlers = read('samples/ZoneWorld/Server/Ops/ops-handlers.ts');
  const opsModule = read('samples/ZoneWorld/Server/Ops/ops-module.ts');
  assert.match(zoneNode, /addRouteMesh\(ZoneWorldNames\.zoneMesh\)[\s\S]*?setRoutingIdPrefix\('zn'\)/);
  assert.doesNotMatch(zoneNode, /useAllocatedRoutingId|setRoutingIdAllocationGroup|\.setRoutingId\(/);
  for (const module of [zoneNode, gateway, ops]) {
    assert.equal((module.match(/\.addRouteMesh\(/g) ?? []).length, 1);
  }
  for (const channel of ['bridgeMesh', 'reportChannel']) {
    assert.match(zoneNode, new RegExp(`zoneMesh\\.channel\\(ZoneWorldNames\\.${channel}\\)`));
  }
  assert.match(zoneNode, /membership\.addHandlerGroup\('zone-ops'\)/);
  assert.match(gateway, /zoneMesh\.objects\(\)\.client\(\)/);
  assert.match(ops, /channel\(ZoneWorldNames\.reportChannel\)\.server\(\)\.addHandlerGroup\('ops'\)/);
  assert.match(nodeHandlers, /@zlinkRequestHandler\('zone-ops', PacketNames\.applyNodeMaintenanceReq\)/);
  assert.doesNotMatch(nodeHandlers, /ensurePlayerActor|nodeRid/);
  assert.match(opsHandlers, /@zlinkSendHandler\('ops', PacketNames\.reportNodeStatusMsg\)/);
  assert.match(opsHandlers, /ZLinkRouteSendHandler<ReportNodeStatusMsg>/);
  assert.match(opsHandlers, /sourceNodeRid/);
  assert.match(opsModule, /OpsRuntimeStatusObserver/);
  assert.doesNotMatch(zoneNode, /addRouteMesh\(ZoneWorldNames\.(?:bridgeMesh|reportChannel)\)/);
});

test('ZoneWorld runner proves the canonical scenario with generated routing identities', () => {
  const runner = read('samples/ZoneWorld/Runner/sample-runner.mjs');
  const client = read('samples/ZoneWorld/Client/main.ts');
  const joinReadiness = read('samples/ZoneWorld/Client/join-readiness.ts');
  const specialClient = read('samples/ZoneWorld/Client/special.ts');
  const contracts = read('samples/ZoneWorld/Shared/contracts.ts');
  const playerHandlers = read('samples/ZoneWorld/Server/ZoneNode/Infrastructure/ZLink/Handlers/player-handlers.ts');
  const zoneSpot = read('samples/ZoneWorld/Server/ZoneNode/Infrastructure/ZLink/Spots/zone-spot.ts');
  const zoneNodeMain = read('samples/ZoneWorld/Server/ZoneNode/main.ts');
  for (const marker of [
    'topology=ready',
    'zoneworld-transfer=completed',
    'zoneworld-border-sync=completed',
    'zoneworld-ops-observe=completed',
    'zoneworld-ops-announce=completed',
    'zoneworld-ops-maintenance=completed',
    'zoneworld=completed'
  ]) assert.match(runner, new RegExp(marker));
  for (const gate of ['ZW-G1', 'ZW-G3', 'ZW-G4', 'ZW-G5']) {
    assert.match(runner, new RegExp(gate));
  }
  for (const scenario of ['C4', 'B4-C2-C3', 'D2', 'E', 'E5-arm', 'E5', 'F']) {
    assert.match(runner, new RegExp(`specialClientConfig\\([^\\n]*?'${scenario}'(?:,[^\\n]*)?\\)`));
  }
  assert.match(runner, /faultTickZone:\s*'zone-nw'/);
  const botTransferProof = runner.indexOf('await waitForCrossOwnerBot(ctx, layout.nodes)');
  const botClientStart = runner.indexOf(
    "specialClientConfig(ctx, shared, gateway, ops, 'F')"
  );
  assert.ok(botTransferProof >= 0, 'ZW-F2 must wait for an Ops-discovered cross-owner bot transition.');
  assert.ok(botTransferProof < botClientStart, 'ZW-F2 must be proven before the F scenario client connects.');
  assert.match(runner, /assertGeneratedRoutingIds\(layout\.pair\.sourceOwnerNodeRid, layout\.pair\.targetOwnerNodeRid\)/);
  assert.match(runner, /await ctx\.start\('target-after-failure'/);
  assert.doesNotMatch(runner, /WaitingForSlot|listRoutingIdSlots|slot=|routing allocation/);
  assert.match(
    zoneSpot,
    /addTimer\(\s*['"]bot-tick['"][\s\S]*?stopOnUnhandledException:\s*false/
  );
  assert.match(zoneSpot, /tickBots\(\): void \{[\s\S]*?if \(this\.botTickTask !== undefined\) return;[\s\S]*?this\.runBotTicks\(\)/);
  assert.match(zoneSpot, /private async runBotTicks\(\): Promise<void> \{[\s\S]*?for \(const actor[\s\S]*?await this\.actorClient\.sendToActor\(/);
  assert.match(contracts, /class BotTickMsg \{\}/);
  assert.doesNotMatch(contracts, /BotTickReq|BotTickRes/);
  assert.match(playerHandlers, /@zlinkSpotActorSendHandler\([\s\S]*?packetName: PacketNames\.botTickMsg/);
  assert.doesNotMatch(playerHandlers, /packetName: PacketNames\.botTickReq/);
  assert.match(specialClient, /representatives = \[BotIds\.northEastX, BotIds\.southWestY\]/);
  assert.doesNotMatch(specialClient, /moved\.size < bots\.size/);
  assert.match(zoneSpot, /if \(!this\.nodeState\.canTickBots\(\)\) return;/);
  const borderPublish = zoneSpot.indexOf('new ZoneBorderEvent(');
  const boundClientPush = zoneSpot.indexOf('new ZoneStateNotify(');
  assert.ok(borderPublish >= 0 && borderPublish < boundClientPush,
    'border synchronization must be admitted before a bound client push');
  const borderObserverReady = client.indexOf('const adjacentViewTask = westObserver');
  const crossNodeMove = client.indexOf('new MoveMsg(boundary.targetInside.x, boundary.targetInside.y)');
  assert.ok(borderObserverReady >= 0 && borderObserverReady < crossNodeMove,
    'the adjacent-zone observer must be ready before the cross-node move publishes its border event');
  const ownedStateWait = joinReadiness.indexOf('.waitFor<ZoneStateNotify>');
  const joinCompletionWait = joinReadiness.indexOf('.waitFor<JoinWorldRes>');
  const joinRequest = joinReadiness.indexOf('.send(new JoinWorldReq(playerId))');
  assert.ok(joinCompletionWait >= 0 && joinCompletionWait < joinRequest,
    'JoinWorldRes must be armed before the one-way JoinWorldReq is submitted');
  assert.ok(ownedStateWait >= 0 && ownedStateWait < joinRequest,
    'accepted joins must arm the owned-state wait before submitting JoinWorldReq');
  for (const playerId of ['player-a1', 'player-a3', 'player-b1-west']) {
    assert.match(client, new RegExp(`joinAndWaitForOwnedState\\([^\\n]+['"]${playerId}['"]`));
  }
  for (const playerId of ['player-b4-west', 'player-b4-east', 'player-e1', 'player-f']) {
    assert.match(specialClient, new RegExp(`joinAndWaitForOwnedState\\([^\\n]+['"]${playerId}['"]`));
  }
  assert.match(specialClient, /join\(newcomer, 'player-e3'\)/);
  assert.match(client, /player\.playerId === joined\.playerId && player\.zoneId === pair\.targetZoneId/);
  assert.match(zoneNodeMain, /await spawnBots\(app, zones\);[\s\S]*?state\.enableBotTicks\(\);/);
  assert.match(
    zoneNodeMain,
    /await spawnBots\(app, zones\);[\s\S]*?bot-start=ready[\s\S]*?waitForBotStart\(node\.botStartSignalPath\)[\s\S]*?state\.enableBotTicks\(\)/
  );
  assert.match(runner, /waitLog\('zone-node-1', 'bot-start=ready'\)[\s\S]*?writeFileSync\(botStartSignalPath/);
});

test('ZoneWorld replacements start empty and prove on-demand creation after the crash boundary', () => {
  const runner = read('samples/ZoneWorld/Runner/sample-runner.mjs');
  const specialClient = read('samples/ZoneWorld/Client/special.ts');
  const zoneNodeMain = read('samples/ZoneWorld/Server/ZoneNode/main.ts');
  const gatewayProbes = read('samples/ZoneWorld/Server/Gateway/relocation-probe-handlers.ts');

  assert.match(
    runner,
    /target-after-failure'[\s\S]*?bootstrapZones:\s*false[\s\S]*?topology=ready node=\$\{targetNode\.nodeId\} zones=/
  );
  assert.match(
    runner,
    /target-after-maintenance'[\s\S]*?bootstrapZones:\s*false[\s\S]*?topology=ready node=\$\{targetNode\.nodeId\} zones=/
  );
  assert.match(runner, /specialClientConfig\([\s\S]*?'G4',[\s\S]*?targetNode\.nodeId/);
  assert.match(runner, /specialClientConfig\([\s\S]*?'G3',[\s\S]*?targetNode\.nodeId/);
  assert.match(runner, /assertFreshReplacementProof\([\s\S]*?layout\.pair\.targetOwnerNodeRid/);
  assert.match(runner, /assertFreshReplacementProof\([\s\S]*?crashProof\.nodeRid/);
  assert.match(runner, /stopAndWaitForLocationLease\([\s\S]*?'target-after-failure',[\s\S]*?'SIGTERM'/);
  assert.doesNotMatch(runner, /runRoutingProbes|recordVerdict\(verdicts, 'ZW-G[34]'\);\s*ctx\.signal/);

  assert.match(
    specialClient,
    /MessageFollowProbeReq\(targetJoin\.playerId, 'zw-g4-crashed-owner'[\s\S]*?actorUnavailable/
  );
  assert.match(specialClient, /node\.zones\.length === 0/);
  assert.match(
    specialClient,
    /CreateFreshActorProbeReq\(actorId\)[\s\S]*?created\.error === null[\s\S]*?ActorLocationProbeReq\(actorId\)[\s\S]*?fresh-actor-proof=/
  );
  assert.match(specialClient, /node\.registered && node\.connected && node\.zones\.length === 0/);
  assert.match(
    gatewayProbes,
    /class CreateFreshActorProbeHandler[\s\S]*?\.getOrCreate\(request\.actorId, ZoneWorldNames\.playerActorType\)[\s\S]*?\.inMesh\(ZoneWorldNames\.zoneMesh\)[\s\S]*?\.submit\(\)/
  );
  assert.match(zoneNodeMain, /new ReportNodeStatusMsg\([\s\S]*?\[\.\.\.state\.zones\(\)\]/);
  assert.match(runner, /zoneCapacity:\s*2/);
});

test('ZoneWorld applies the Node sample configuration policy without environment settings', () => {
  const configuration = read('samples/ZoneWorld/Server/Configuration/configuration.ts');
  const runner = read('samples/ZoneWorld/Runner/sample-runner.mjs');
  const sampleSources = fs.readdirSync(path.join(nodeRoot, 'samples/ZoneWorld'), {
    recursive: true,
    withFileTypes: true
  }).filter((entry) => entry.isFile() && /\.(?:ts|mjs|sh)$/.test(entry.name))
    .map((entry) => read(path.join(entry.parentPath, entry.name).slice(nodeRoot.length + 1)))
    .join('\n');

  assert.match(configuration, /ConfigModule\.forRoot\(/);
  assert.match(configuration, /skipProcessEnv:\s*true/);
  assert.match(configuration, /validateConfiguration\(service\.get\('zoneworld'\), role\)/);
  assert.match(configuration, /--config <path> is the only supported framework host argument/);
  assert.match(runner, /\['--config',\s*[^\]]+\]/);
  assert.doesNotMatch(sampleSources, /process\.env/);
});

test('ZoneWorld maintenance rejects every arrival while allowing same-zone movement', () => {
  const { NodeRuntimeState } = require(path.join(
    nodeRoot,
    'samples/ZoneWorld/dist/Server/ZoneNode/Domain/node-runtime-state.js'
  ));
  const state = new NodeRuntimeState({ zoneNode: { nodeId: 'zone-node-1', zoneCapacity: 2 } });
  state.setMaintenance('zone-node-1', true);

  assert.equal(state.rejectsArrival(), true);

  state.joined('player-1', 'zone-nw');
  state.joined('player-1', 'zone-sw');
  state.left('player-1', 'zone-nw');
  assert.equal(state.playerCount(), 1, 'overlapping membership retains the player after one source leave');
  state.left('player-1', 'zone-sw');
  assert.equal(state.playerCount(), 0);
});

test('ZoneWorld human state pushes cross the ActorRef boundary before the bound session', () => {
  const actor = read('samples/ZoneWorld/Server/ZoneNode/Infrastructure/ZLink/Actors/player-actor.ts');
  const spot = read('samples/ZoneWorld/Server/ZoneNode/Infrastructure/ZLink/Spots/zone-spot.ts');
  assert.match(actor, /push\(payload: unknown\): void/);
  assert.match(actor, /this\.context\.boundSession\.send\(payload\)\.submit\(\)/);
  assert.doesNotMatch(actor, /await[\s\S]*?boundSession\.send/);
  assert.match(spot, /sendToActor\(actorId, new DeliverZoneNotificationMsg\(payload\)\)/);
  assert.doesNotMatch(spot, /Map<string, PlayerActor>/);
  assert.doesNotMatch(spot, /actor\.push\(/);
});

test('ZoneWorld Ops translates public diagnostics requests to the node channel contract', () => {
  const handlers = read('samples/ZoneWorld/Server/Ops/ops-handlers.ts');
  assert.match(
    handlers,
    /requestToChannel\([\s\S]*?ZoneWorldNames\.opsChannel\(request\.nodeId\),[\s\S]*?new GetNodeDiagnosticsReq\(request\.nodeId\)/
  );
});

test('ZoneWorld node status combines logical identity with live routing status', () => {
  const { NodeRegistry } = require(path.join(
    nodeRoot,
    'samples/ZoneWorld/dist/Server/Ops/node-registry.js'
  ));
  const registry = new NodeRegistry();
  const { ZoneWorldSpec } = require(path.join(
    nodeRoot,
    'samples/ZoneWorld/dist/Shared/spec.js'
  ));
  registry.applyLiveRoutingIds(new Set(['zn-1']));
  registry.report({
    nodeId: 'zone-node-1',
    maintenance: false,
    zones: ['zone-nw', 'zone-sw'],
    playerCount: 0
  }, 'zn-1');

  assert.deepEqual(registry.snapshot().map(({ registered, connected }) => ({ registered, connected })), [
    { registered: true, connected: true }
  ]);
  assert.deepEqual(
    registry.applyLiveRoutingIds(new Set()).map(({ registered, connected }) => ({ registered, connected })),
    [{ registered: true, connected: false }]
  );
  assert.deepEqual(
    registry.expireReports(Date.now() + ZoneWorldSpec.nodeStatusReportTtlMs)
      .map(({ registered, connected }) => ({ registered, connected })),
    [{ registered: false, connected: false }]
  );
});

test('ZoneWorld alert replay is bounded and begins only when WatchNodes is handled', () => {
  const registry = read('samples/ZoneWorld/Server/Ops/ops-console-registry.ts');
  const handlers = read('samples/ZoneWorld/Server/Ops/ops-handlers.ts');
  assert.match(registry, /const RECENT_ALERT_COUNT = 20/);
  assert.match(registry, /add\(context: ZLinkSessionContext\): void \{\s*this\.consoles\.set\(context\.sessionId, context\);\s*}/);
  assert.match(handlers, /reply\(new WatchNodesRes[\s\S]*?this\.consoles\.replayAlerts\(context\)/);
});
