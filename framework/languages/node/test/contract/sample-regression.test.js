const assert = require('node:assert/strict');
const childProcess = require('node:child_process');
const fs = require('node:fs');
const path = require('node:path');
const test = require('node:test');

const workspaceRoot = path.resolve(__dirname, '..', '..');
const samplesRoot = path.join(workspaceRoot, 'samples');
const commonSampleDocsRoot = path.resolve(workspaceRoot, '..', '..', 'doc', 'framework', 'common', 'sample');
const requiredSamples = [
  'TicTacToe.Ts',
  'Bingo.Ts',
  'DeliveryDispatch.Ts',
  'SupportChat.Ts',
  'GameQuest.Ts',
  'ShoppingMall.Ts'
];
const maintainedSamples = [...requiredSamples, 'ZoneWorld'];
const topologySamples = [
  'TicTacToe.Ts',
  'Bingo.Ts',
  'DeliveryDispatch.Ts',
  'SupportChat.Ts',
  'GameQuest.Ts',
  'ShoppingMall.Ts'
];

test('node samples define required runners and keep scenario contracts in common documents', () => {
  const missing = [];
  const samplesReadme = path.join(samplesRoot, 'README.ko.md');
  if (!fs.existsSync(samplesReadme)) {
    missing.push('samples/README.ko.md');
  } else {
    const readme = fs.readFileSync(samplesReadme, 'utf8');
    for (const requiredText of [
      '../../../doc/framework/common/sample/README.ko.md',
      'npm run browser:install',
      './samples/run_samples.sh',
      './samples/run_samples.ps1',
      'Bingo.Ts/Client/bingo-client-scenario.ts'
    ]) {
      if (!readme.includes(requiredText)) {
        missing.push(`samples/README.ko.md missing '${requiredText}'`);
      }
    }
  }
  for (const sample of requiredSamples) {
    for (const relative of ['package.json', 'run_sample.sh', 'run_sample.ps1']) {
      const target = path.join(samplesRoot, sample, relative);
      if (!fs.existsSync(target)) {
        missing.push(`${sample}/${relative}`);
      }
    }
    for (const obsolete of ['README.ko.md', 'sample-porting-inventory.ko.md']) {
      if (fs.existsSync(path.join(samplesRoot, sample, obsolete))) {
        missing.push(`${sample}/${obsolete} must not duplicate the common sample specification`);
      }
    }
  }
  if (!fs.existsSync(path.join(commonSampleDocsRoot, 'README.ko.md'))) {
    missing.push('framework/doc/framework/common/sample/README.ko.md');
  }
  if (!fs.existsSync(path.join(samplesRoot, 'run_samples.sh'))) {
    missing.push('run_samples.sh');
  }
  if (fs.existsSync(path.join(samplesRoot, 'shared'))) {
    missing.push('samples/shared must not hide sample logic');
  }

  assert.deepEqual(missing, []);
});

test('node topology samples implement the common sample role layout', () => {
  const expected = {
    'TicTacToe.Ts': [
      'Client/tictactoe-client-scenario.ts',
      'Client/main.ts',
      'Server/Api/Handlers/authenticate-player-handler.ts',
      'Server/Api/Handlers/create-game-http-handler.ts',
      'Server/Api/main.ts',
      'Server/Play/Domain/TicTacToe/tictactoe-board.ts',
      'Server/Play/Domain/TicTacToe/tictactoe-match.ts',
      'Server/Play/Infrastructure/ZLink/Actors/play-actor.ts',
      'Server/Play/Infrastructure/ZLink/Actors/play-actor-factory.ts',
      'Server/Play/Infrastructure/ZLink/Sessions/play-session.ts',
      'Server/Play/Infrastructure/ZLink/Sessions/play-session-factory.ts',
      'Server/Play/Infrastructure/ZLink/Spots/EntrySpot/Handlers/play-actor-join-game-handler.ts',
      'Server/Play/Infrastructure/ZLink/Spots/TicTacToeGameSpot/Handlers/play-actor-place-mark-handler.ts',
      'Server/Play/Infrastructure/ZLink/Spots/TicTacToeGameSpot/Handlers/tictactoe-game-timer-handler.ts',
      'Server/Play/Infrastructure/ZLink/Spots/EntrySpot/play-entry-spot.ts',
      'Server/Play/Infrastructure/ZLink/Spots/TicTacToeGameSpot/tictactoe-game-spot.ts',
      'Server/Play/main.ts',
      'Shared/Contracts/messages.ts'
    ],
    'Bingo.Ts': [
      'Client/bingo-client-scenario.ts',
      'Client/main.ts',
      'Server/Api/Handlers/authenticate-player-handler.ts',
      'Server/Api/Handlers/match-bingo-handler.ts',
      'Server/Api/main.ts',
      'Server/Play/Domain/Bingo/bingo-card.ts',
      'Server/Play/Domain/Bingo/bingo-game.ts',
      'Server/Play/Domain/Bingo/bingo-room-game.ts',
      'Server/Play/Domain/Bingo/bingo-room-models.ts',
      'Server/Matchmaking/bingo-matchmaking-module.ts',
      'Server/Matchmaking/bingo-matchmaker.ts',
      'Server/Matchmaking/bingo-match-reservation-store.ts',
      'Server/Play/Infrastructure/ZLink/Actors/player-actor.ts',
      'Server/Play/Infrastructure/ZLink/Actors/player-actor-factory.ts',
      'Server/Play/Infrastructure/ZLink/Handlers/ensure-player-actor-handler.ts',
      'Server/Play/Infrastructure/ZLink/Spots/BingoRoomSpot/Handlers/bingo-room-timer-handler.ts',
      'Server/Play/Infrastructure/ZLink/Spots/EntrySpot/Handlers/match-bingo-actor-handler.ts',
      'Server/Play/Infrastructure/ZLink/Spots/EntrySpot/Handlers/observe-bingo-events-handler.ts',
      'Server/Play/Infrastructure/ZLink/Spots/BingoRoomSpot/Handlers/stop-observing-bingo-events-handler.ts',
      'Server/Play/Infrastructure/ZLink/Spots/BingoRoomSpot/Handlers/submit-bingo-card-handler.ts',
      'Server/Play/Infrastructure/ZLink/Spots/EntrySpot/bingo-entry-spot.ts',
      'Server/Play/Infrastructure/ZLink/Spots/BingoRoomSpot/bingo-room-spot.ts',
      'Server/Play/main.ts',
      'Server/Session/Sessions/Handlers/authenticate-session-handler.ts',
      'Server/Session/Sessions/bingo-session.ts',
      'Server/Session/main.ts',
      'Server/Configuration/location-store.ts',
      'Server/Configuration/sample-names.ts',
      'Shared/Contracts/bingo_messages.proto',
      'Shared/Contracts/protobuf-codec.ts',
      'Shared/Contracts/messages.ts'
    ],
    'SupportChat.Ts': [
      'Client/supportchat-client-scenario.ts',
      'Client/main.ts',
      'Client/Configuration/sample-config.ts',
      'Client/Configuration/sample-names.ts',
      'Server/Api/Handlers/open-conversation-handler.ts',
      'Server/Api/Handlers/authenticate-user-handler.ts',
      'Server/Api/supportchat-api-module.ts',
      'Server/Api/main.ts',
      'Server/Support/Domain/SupportChat/conversation.ts',
      'Server/Support/Domain/SupportChat/conversation-models.ts',
      'Server/Support/Domain/SupportChat/conversation-events.ts',
      'Server/Support/Domain/SupportChat/conversation-policy.ts',
      'Server/Support/Application/ConversationAssignment/support-conversation-allocator.ts',
      'Server/Support/Application/ConversationAssignment/agent-availability-directory.ts',
      'Server/Support/Application/ConversationAssignment/agent-assignment-service.ts',
      'Server/Support/Infrastructure/ZLink/Actors/support-user-actor.ts',
      'Server/Support/Infrastructure/ZLink/Actors/support-user-actor-factory.ts',
      'Server/Support/Infrastructure/ZLink/Spots/ConversationSpot/Notifications/conversation-event-mapper.ts',
      'Server/Support/Infrastructure/ZLink/Spots/ConversationSpot/Notifications/support-notification-publisher.ts',
      'Server/Support/Infrastructure/ZLink/Spots/ConversationSpot/conversation-create-request.ts',
      'Server/Support/Infrastructure/ZLink/Spots/EntrySpot/support-entry-spot.ts',
      'Server/Support/Infrastructure/ZLink/Spots/ConversationSpot/conversation-spot.ts',
      'Server/Support/Infrastructure/ZLink/Spots/ConversationSpot/Handlers/conversation-idle-timer-handler.ts',
      'Server/Support/Infrastructure/ZLink/Spots/ConversationSpot/Handlers/conversation-actor-handlers.ts',
      'Server/Support/Infrastructure/ZLink/Spots/EntrySpot/support-entry-handlers.ts',
      'Server/Support/supportchat-support-module.ts',
      'Server/Support/main.ts',
      'Server/Session/Sessions/supportchat-session.ts',
      'Server/Session/supportchat-session-module.ts',
      'Server/Session/main.ts',
      'Server/Configuration/sample-config.ts',
      'Server/Configuration/sample-names.ts',
      'Server/runtime-support.ts',
      'Shared/Contracts/messages.ts'
    ],
    'DeliveryDispatch.Ts': [
      'Client/deliverydispatch-client-scenario.ts',
      'Client/main.ts',
      'Server/Dispatch/main.ts',
      'Server/Tracking/main.ts',
      'Server/Session/main.ts',
      'Server/CourierSession/main.ts',
      'Server/Courier/node1-main.ts',
      'Server/Courier/node2-main.ts',
      'Server/Courier/courier-actor.ts',
      'Server/Courier/courier-entry-spot.ts',
      'Server/Courier/courier-module.ts',
      'Server/Courier/offer-delivery-handler.ts',
      'Server/CourierSession/courier-session.ts',
      'Server/CourierSession/courier-session-module.ts',
      'Server/DispatchCenter/dispatch-center-module.ts',
      'Server/DispatchCenter/dispatch-worker.ts',
      'Server/Session/customer-session.ts',
      'Server/Tracking/tracking-module.ts',
      'Shared/Configuration/sample-names.ts',
      'Shared/Contracts/messages.ts'
    ],
    'GameQuest.Ts': [
      'Client/gamequest-client-scenario.ts',
      'Client/main.ts',
      'Server/bootstrap.ts',
      'Server/ApiA/main.ts',
      'Server/ApiB/main.ts',
      'Server/MissionA/main.ts',
      'Server/MissionB/main.ts',
      'Server/GameApi/game-api-module.ts',
      'Server/GameApi/game-api-server.ts',
      'Server/GameApi/Application/gameplay-action-service.ts',
      'Server/GameApi/Infrastructure/ZLink/gameplay-event-publisher.ts',
      'Server/QuestMission/Infrastructure/ZLink/player-quest-spot-provisioner.ts',
      'Server/QuestMission/Infrastructure/ZLink/Spots/PlayerQuestSpot/player-quest-spot.ts',
      'Server/Shared/Store/quest-progress-store.ts',
      'Shared/Configuration/sample-names.ts',
      'Shared/Contracts/messages.ts'
    ],
    'ShoppingMall.Ts': [
      'Client/shoppingmall-client-scenario.ts',
      'Client/main.ts',
      'Server/bootstrap.ts',
      'Server/ApiA/main.ts',
      'Server/ApiB/main.ts',
      'Server/WorkflowA/main.ts',
      'Server/WorkflowB/main.ts',
      'Server/Shared/Store/order-store.ts',
      'Shared/Configuration/sample-names.ts',
      'Shared/Contracts/messages.ts'
    ]
  };
  const missing = [];
  for (const [sample, relatives] of Object.entries(expected)) {
    if (!topologySamples.includes(sample)) {
      continue;
    }
    for (const relative of relatives) {
      if (!fs.existsSync(path.join(samplesRoot, sample, relative))) {
        missing.push(`${sample}/${relative}`);
      }
    }
  }

  assert.deepEqual(missing, []);
});

test('GameQuest TypeScript sample registers required sample and provisions player quest spots', () => {
  assert.ok(requiredSamples.includes('GameQuest.Ts'));

  const names = readSample('GameQuest.Ts', 'Shared/Configuration/sample-names.ts');
  const publisher = readSample('GameQuest.Ts', 'Server/GameApi/Infrastructure/ZLink/gameplay-event-publisher.ts');
  const sessionModule = readSample('GameQuest.Ts', 'Server/GameApi/game-api-module.ts');
  const questModule = readSample('GameQuest.Ts', 'Server/QuestMission/gamequest-quest-module.ts');
  const provisioner = readSample(
    'GameQuest.Ts',
    'Server/QuestMission/Infrastructure/ZLink/player-quest-spot-provisioner.ts'
  );
  const spot = readSample(
    'GameQuest.Ts',
    'Server/QuestMission/Infrastructure/ZLink/Spots/PlayerQuestSpot/player-quest-spot.ts'
  );
  const spotHandlers = readSample(
    'GameQuest.Ts',
    'Server/QuestMission/Infrastructure/ZLink/Spots/PlayerQuestSpot/player-quest-spot-handlers.ts'
  );
  const missing = [];

  for (const [content, text] of [
    [names, 'questMissionSpotId(playerId: string)'],
    [publisher, '.sendToSpot(questMissionSpotId(event.playerId), message)'],
    [publisher, '.instanceSpot(SampleNames.playerQuestSpotType)'],
    [publisher, '.inMesh(SampleNames.playerQuestSpotMesh)'],
    [sessionModule, '.addRouteMesh(SampleNames.playerQuestSpotMesh)'],
    [questModule, '.addInstanceSpotFactory(\n            SampleNames.playerQuestSpotType,\n            PlayerQuestSpot,'],
    [provisioner, 'ZLINK_SPOT_OUTBOUND'],
    [provisioner, '.requestToSpot(questMissionSpotId(playerId), request)'],
    [provisioner, '.instanceSpot(SampleNames.playerQuestSpotType)'],
    [spot, 'private aggregate: PlayerQuestAggregate | undefined'],
    [spot, 'ensureAggregate(load: () => PlayerQuestAggregate)'],
    [spotHandlers, 'this.processor.rehydrate(message.playerId)'],
    [spotHandlers, 'spot.replaceAggregate(result.aggregate)'],
    [spotHandlers, 'this.processor.rehydrate(request.playerId)']
  ]) {
    if (!content.includes(text)) {
      missing.push(text);
    }
  }

  assert.deepEqual(missing, []);
});

test('node Bingo and TicTacToe samples implement Entry Spot actor lifecycle flow', () => {
  const files = {
    bingoModule: readSample('Bingo.Ts', 'Server/Play/bingo-play-module.ts'),
    bingoEntry: readSample('Bingo.Ts', 'Server/Play/Infrastructure/ZLink/Spots/EntrySpot/bingo-entry-spot.ts'),
    bingoRoom: readSample('Bingo.Ts', 'Server/Play/Infrastructure/ZLink/Spots/BingoRoomSpot/bingo-room-spot.ts'),
    bingoAllocator: readSample('Bingo.Ts', 'Server/Matchmaking/bingo-match-reservation-store.ts'),
    bingoAllocate: readSample('Bingo.Ts', 'Server/Matchmaking/bingo-matchmaker.ts'),
    bingoEnsureActor: readSample('Bingo.Ts', 'Server/Play/Infrastructure/ZLink/Handlers/ensure-player-actor-handler.ts'),
    bingoApiMatch: readSample('Bingo.Ts', 'Server/Api/Handlers/match-bingo-handler.ts'),
    bingoActorMatch: readSample('Bingo.Ts', 'Server/Play/Infrastructure/ZLink/Spots/EntrySpot/Handlers/match-bingo-actor-handler.ts'),
    bingoActorLifecycle: readSample('Bingo.Ts', 'Server/Play/Infrastructure/ZLink/Actors/player-actor-lifecycle-handlers.ts'),
    ticTacToeModule: readSample('TicTacToe.Ts', 'Server/Play/tictactoe-play-module.ts'),
    ticTacToeEntry: readSample('TicTacToe.Ts', 'Server/Play/Infrastructure/ZLink/Spots/EntrySpot/play-entry-spot.ts'),
    ticTacToeGame: readSample('TicTacToe.Ts', 'Server/Play/Infrastructure/ZLink/Spots/TicTacToeGameSpot/tictactoe-game-spot.ts'),
    ticTacToeCreate: readSample('TicTacToe.Ts', 'Server/Api/Handlers/create-game-http-handler.ts'),
    ticTacToeSession: readSample('TicTacToe.Ts', 'Server/Play/Infrastructure/ZLink/Sessions/play-session.ts'),
    ticTacToeAuthenticate: readSample(
      'TicTacToe.Ts',
      'Server/Play/Infrastructure/ZLink/Sessions/Handlers/authenticate-play-session-handler.ts'
    ),
    ticTacToeActorJoin: readSample('TicTacToe.Ts', 'Server/Play/Infrastructure/ZLink/Spots/EntrySpot/Handlers/play-actor-join-game-handler.ts'),
    ticTacToeActorLeave: readSample('TicTacToe.Ts', 'Server/Play/Infrastructure/ZLink/Spots/TicTacToeGameSpot/Handlers/play-actor-leave-game-handler.ts')
  };
  const missing = [];
  const violations = [];
  for (const [name, content, text] of [
    ['Bingo module', files.bingoModule, '.addSpotFactory(\n            SampleNames.roomSpotType,\n            BingoRoomSpot,'],
    ['Bingo API match', files.bingoApiMatch, 'ZLINK_SPOT_OUTBOUND'],
    ['Bingo API match', files.bingoApiMatch, '.instanceSpot(SampleNames.matchmakerSpotType)'],
    ['Bingo allocate', files.bingoAllocate, 'ReserveBingoRoomHandler'],
    ['Bingo ensure actor', files.bingoEnsureActor, 'ZLINK_ACTOR_MANAGER'],
    ['Bingo actor match', files.bingoActorMatch, '.joinSpot(matched.roomId'],
    ['Bingo entry', files.bingoEntry, 'onCreateActor'],
    ['Bingo entry', files.bingoEntry, 'onJoinedActor'],
    ['Bingo entry', files.bingoEntry, 'destroyActor(actor'],
    ['Bingo room', files.bingoRoom, 'onActorJoin'],
    ['Bingo room', files.bingoRoom, 'onLeaveActor'],
    ['Bingo actor lifecycle', files.bingoActorLifecycle, 'spot.context.leaveActor(actor)'],
    ['TicTacToe module', files.ticTacToeModule, '.addSpotFactory(\n            SampleNames.gameSpotType,\n            TicTacToeGameSpot,'],
    ['TicTacToe create', files.ticTacToeCreate, '.create(SampleNames.gameSpotType)'],
    ['TicTacToe create', files.ticTacToeCreate, '.inMesh(SampleNames.playSpotNode)'],
    ['TicTacToe actor join', files.ticTacToeActorJoin, '.joinSpot(request.roomId, joinRequest)'],
    ['TicTacToe entry', files.ticTacToeEntry, 'onCreateActor'],
    ['TicTacToe entry', files.ticTacToeEntry, 'onJoinedActor'],
    ['TicTacToe entry', files.ticTacToeEntry, 'destroyActor(actor'],
    ['TicTacToe game', files.ticTacToeGame, 'onActorJoin'],
    ['TicTacToe game', files.ticTacToeGame, 'onLeaveActor'],
    ['TicTacToe actor leave', files.ticTacToeActorLeave, 'spot.context.leaveActor(actor)'],
    ['TicTacToe authentication', files.ticTacToeAuthenticate, 'context.actors.bindOrGet(actorRef)'],
    ['TicTacToe session', files.ticTacToeSession, 'await actor.relay(payload)']
  ]) {
    if (!content.includes(text)) {
      missing.push(`${name}:${text}`);
    }
  }
  for (const [name, content, pattern] of [
    ['Bingo allocator', files.bingoAllocator, /ZLINK_SPOT_MANAGER|\.getOrCreate\(|\.executeOnSpot|Infrastructure\/ZLink|RedisBingoMatchQueue/],
    ['Bingo entry', files.bingoEntry, /\.onActorJoin\s*\(/],
    ['TicTacToe entry', files.ticTacToeEntry, /cleanupFinishedRoom|\.onJoinedActor\s*\(/],
    ['TicTacToe session', files.ticTacToeSession, /TicTacToeGameCreator|cleanupFinishedRoom/]
  ]) {
    if (pattern.test(content)) {
      violations.push(name);
    }
  }

  assert.deepEqual(missing, []);
  assert.deepEqual(violations, []);
});

test('node Bingo stop observing request is owned by the observer room Spot', () => {
  const entry = readSample('Bingo.Ts', 'Server/Play/Infrastructure/ZLink/Spots/EntrySpot/bingo-entry-spot.ts');
  const room = readSample('Bingo.Ts', 'Server/Play/Infrastructure/ZLink/Spots/BingoRoomSpot/bingo-room-spot.ts');
  const handler = readSample('Bingo.Ts', 'Server/Play/Infrastructure/ZLink/Spots/BingoRoomSpot/Handlers/stop-observing-bingo-events-handler.ts');

  assert.match(handler, /zlinkSpotActorRequestHandler/);
  assert.match(handler, /spot:\s*\(\)\s*=>\s*BingoRoomSpot/);
  assert.match(handler, /actor:\s*\(\)\s*=>\s*PlayerActor/);
  assert.match(handler, /packetName:\s*PacketNames\.stopObservingBingoEventsReq/);
  assert.match(room, /verifyStopObserving\(/);
  assert.match(handler, /spot\.context\.leaveActor\(actor\)/);
  assert.doesNotMatch(room, /actorRequest\(PacketNames\.stopObservingBingoEventsReq/);
  assert.doesNotMatch(entry, /actorRequest\(PacketNames\.stopObservingBingoEventsReq/);
});

test('node client flow files use ClientScenario names', () => {
  const violations = [];
  for (const sample of requiredSamples) {
    const clientRoot = path.join(samplesRoot, sample, 'Client');
    for (const file of sampleSourceFiles(clientRoot)) {
      const relative = relativePath(path.join(samplesRoot, sample), file);
      const content = fs.readFileSync(file, 'utf8');
      if (/client-app|self-check|TestScenario/.test(relative) || /ClientApp|TestScenario/.test(content)) {
        violations.push(`${sample}/${relative}`);
      }
    }
  }

  assert.deepEqual(violations, []);
});

test('node samples keep only the maintained canonical variants', () => {
  const entries = fs.readdirSync(samplesRoot, { withFileTypes: true })
    .filter((entry) => entry.isDirectory())
    .map((entry) => entry.name)
    .sort();

  assert.deepEqual(entries, [...maintainedSamples].sort());
  assert.equal(entries.some((entry) => /SessionGateway|Gateway|StreamingClient/.test(entry)), false);
});

test('node common-spec samples expose buildable scenario entrypoints', () => {
  const cases = [
    [
      'DeliveryDispatch.Ts',
      '@zlink-systems/sample-deliverydispatch-ts',
      'deliverydispatch-client-scenario.ts',
      'DeliveryDispatchClientScenario',
      'PASS DeliveryDispatch.Ts',
      [
        'Server/Dispatch/main.ts',
        'Server/Tracking/main.ts',
        'Server/Session/main.ts',
        'Server/CourierSession/main.ts',
        'Server/Courier/node1-main.ts',
        'Server/Courier/node2-main.ts'
      ]
    ],
    [
      'GameQuest.Ts',
      '@zlink-systems/sample-gamequest-ts',
      'gamequest-client-scenario.ts',
      'GameQuestClientScenario',
      'PASS GameQuest.Ts',
      [
        'Server/ApiA/main.ts',
        'Server/ApiB/main.ts',
        'Server/MissionA/main.ts',
        'Server/MissionB/main.ts'
      ]
    ],
    [
      'ShoppingMall.Ts',
      '@zlink-systems/sample-shoppingmall-ts',
      'shoppingmall-client-scenario.ts',
      'ShoppingMallClientScenario',
      'PASS ShoppingMall.Ts',
      [
        'Server/ApiA/main.ts',
        'Server/ApiB/main.ts',
        'Server/WorkflowA/main.ts',
        'Server/WorkflowB/main.ts'
      ]
    ]
  ];
  const missing = [];

  for (const [sample, packageName, scenarioFile, scenarioName, passMarker, serverEntries] of cases) {
    const packageJson = fs.readFileSync(path.join(samplesRoot, sample, 'package.json'), 'utf8');
    const tsconfig = fs.readFileSync(path.join(samplesRoot, sample, 'tsconfig.json'), 'utf8');
    const client = fs.readFileSync(path.join(samplesRoot, sample, 'Client', 'main.ts'), 'utf8');
    const scenario = fs.readFileSync(path.join(
      samplesRoot,
      sample,
      'Client',
      scenarioFile
    ), 'utf8');
    const contracts = fs.readFileSync(path.join(samplesRoot, sample, 'Shared', 'Contracts', 'messages.ts'), 'utf8');

    for (const [content, text] of [
      [packageJson, packageName],
      [packageJson, 'tsc -p tsconfig.json'],
      [tsconfig, '"outDir": "dist"'],
      [tsconfig, '"Server/**/*.ts"'],
      [client, scenarioName],
      [client, passMarker],
      [scenario, 'ensure('],
      [contracts, 'PacketNames']
    ]) {
      if (!content.includes(text)) {
        missing.push(`${sample}:${text}`);
      }
    }
    for (const serverEntry of serverEntries) {
      const entryPath = path.join(samplesRoot, sample, serverEntry);
      if (!fs.existsSync(entryPath)) {
        missing.push(`${sample}:${serverEntry}`);
      }
    }
  }

  assert.deepEqual(missing, []);
});

test('SupportChat uses managers for object creation and keeps API authentication on a channel', () => {
  const allocator = readSample(
    'SupportChat.Ts',
    'Server/Support/Application/ConversationAssignment/support-conversation-allocator.ts'
  );
  const assignment = readSample(
    'SupportChat.Ts',
    'Server/Support/Application/ConversationAssignment/agent-assignment-service.ts'
  );
  const apiHandler = fs.readFileSync(
    path.join(samplesRoot, 'SupportChat.Ts', 'Server', 'Api', 'Handlers', 'open-conversation-handler.ts'),
    'utf8'
  );
  const apiModule = readSample('SupportChat.Ts', 'Server/Api/supportchat-api-module.ts');
  const supportModule = readSample('SupportChat.Ts', 'Server/Support/supportchat-support-module.ts');
  const sessionModule = readSample('SupportChat.Ts', 'Server/Session/supportchat-session-module.ts');

  assert.match(allocator, /new Conversation\(/);
  assert.doesNotMatch(allocator, /ZLink|requestToChannel|actors\.get/);
  assert.match(assignment, /assignNextAgent\(\)/);
  assert.doesNotMatch(assignment, /ZLink|requestToChannel|actors\.get/);

  assert.match(apiHandler, /ZLINK_SPOT_MANAGER/);
  assert.match(apiHandler, /\.create\(SampleNames\.conversationSpotType\)/);
  assert.match(apiHandler, /\.inMesh\(SampleNames\.meshName\)/);
  assert.doesNotMatch(apiHandler, /requestToChannel|supportChannel|nodeRid/);
  for (const module of [apiModule, supportModule, sessionModule]) {
    assert.equal((module.match(/\.addRouteMesh\(/g) ?? []).length, 1);
    assert.match(module, /\.addRouteMesh\(SampleNames\.conversationSpotMesh\)/);
  }
  assert.match(apiModule, /\.listen\(config\.apiChannelEndpoint\)[\s\S]*\.setRoutingIdPrefix\('support-api'\)/);
  assert.match(supportModule, /\.listen\(config\.supportSpotEndpoint\)\.setRoutingIdPrefix\('support-owner'\)/);
  assert.match(sessionModule, /\.listen\(config\.sessionSpotEndpoint\)\.setRoutingIdPrefix\('support-session'\)/);
  assert.match(apiModule, /addClientServerChannel\(SampleNames\.apiChannel\)[\s\S]*\.server\(\)[\s\S]*\.addHandlerGroup\('api'\)/);
  assert.match(supportModule, /addClientServerChannel\(SampleNames\.apiChannel\)\.client\(\)/);
  assert.match(sessionModule, /addClientServerChannel\(SampleNames\.apiChannel\)\.client\(\)/);
  assert.match(apiModule, /mesh\.objects\(\)\.client\(\)/);
  assert.match(sessionModule, /mesh\.objects\(\)\.client\(\)/);
  assert.doesNotMatch(supportModule, /supportChannel|ensureSupportUserActor|allocateConversation/);
});

test('DeliveryDispatch TypeScript sample uses framework channel topology', () => {
  const clientScenario = readSample('DeliveryDispatch.Ts', 'Client/deliverydispatch-client-scenario.ts');
  const dispatchMain = readSample('DeliveryDispatch.Ts', 'Server/Dispatch/main.ts');
  const dispatchCenterModule = readSample('DeliveryDispatch.Ts', 'Server/DispatchCenter/dispatch-center-module.ts');
  const courierModule = readSample('DeliveryDispatch.Ts', 'Server/Courier/courier-module.ts');
  const courierSessionModule = readSample('DeliveryDispatch.Ts', 'Server/CourierSession/courier-session-module.ts');
  const courierSession = readSample('DeliveryDispatch.Ts', 'Server/CourierSession/courier-session.ts');
  const customerSession = readSample('DeliveryDispatch.Ts', 'Server/Session/customer-session.ts');
  const names = readSample('DeliveryDispatch.Ts', 'Shared/Configuration/sample-names.ts');
  const trackingModule = readSample('DeliveryDispatch.Ts', 'Server/Tracking/tracking-module.ts');
  const sessionModule = readSample('DeliveryDispatch.Ts', 'Server/Session/session-module.ts');
  const dispatchWorker = readSample('DeliveryDispatch.Ts', 'Server/DispatchCenter/dispatch-worker.ts');
  const offerHandler = readSample('DeliveryDispatch.Ts', 'Server/Courier/offer-delivery-handler.ts');
  const messages = readSample('DeliveryDispatch.Ts', 'Shared/Contracts/messages.ts');
  const serverEntries = [
    'Server/Dispatch/main.ts',
    'Server/Tracking/main.ts',
    'Server/Session/main.ts',
    'Server/CourierSession/main.ts',
    'Server/Courier/node1-main.ts',
    'Server/Courier/node2-main.ts'
  ].map((file) => readSample('DeliveryDispatch.Ts', file)).join('\n');
  const runSample = fs.readFileSync(path.join(samplesRoot, 'DeliveryDispatch.Ts', 'run_sample.sh'), 'utf8');
  const sampleRunner = readSample('DeliveryDispatch.Ts', 'Runner/sample-runner.mjs');

  assert.match(clientScenario, /BrowserHttpClient/);
  assert.match(clientScenario, /\.fetch<CreateDeliveryRes>\(\)/);
  assert.match(clientScenario, /\.fetch<ServerAssertionRes>\(\)/);
  assert.match(clientScenario, /customer\.request\(subscribeDelivery/);
  assert.match(clientScenario, /waitForSequence<DeliveryStatusNotify>/);
  assert.match(dispatchMain, /startDispatchApi\(center, config/);
  assert.doesNotMatch(dispatchMain, /createDispatchApiModule|const api = await NestFactory/);
  for (const module of [dispatchCenterModule, courierModule, courierSessionModule, trackingModule, sessionModule]) {
    assert.equal((module.match(/\.addRouteMesh\(/g) ?? []).length, 1);
  }
  for (const module of [dispatchCenterModule, courierModule, courierSessionModule]) {
    assert.match(module, /\.addRouteMesh\(SampleNames\.courierMeshName\)/);
  }
  for (const module of [trackingModule, sessionModule]) {
    assert.match(module, /\.addRouteMesh\(SampleNames\.customerMeshName\)/);
  }
  assert.match(dispatchCenterModule, /addClientServerChannel\(SampleNames\.dispatchChannel\)[\s\S]*\.server\(\)/);
  assert.match(dispatchCenterModule, /addClientServerChannel\(SampleNames\.trackingChannel\)\.client\(\)/);
  assert.match(courierModule, /addClientServerChannel\(SampleNames\.dispatchChannel\)\.client\(\)/);
  assert.match(trackingModule, /addClientServerChannel\(SampleNames\.trackingChannel\)[\s\S]*\.server\(\)/);
  assert.match(dispatchCenterModule, /\.listen\(config\.dispatchSpotEndpoint\)/);
  assert.match(sessionModule, /\.addStreamNode\(SampleNames\.customerStreamNode\)/);
  assert.match(courierSession, /\.getOrCreate\(courierId, SampleNames\.courierActorType\)/);
  assert.match(courierSession, /bindOrGet\(actorRef\)/);
  assert.match(customerSession, /\.getOrCreate\(CustomerId, SampleNames\.customerActorType\)/);
  assert.match(names, /courierMeshName: 'deliverydispatch\.courier'/);
  assert.match(names, /customerMeshName: 'deliverydispatch\.customer'/);
  assert.match(serverEntries, /NestFactory\.createApplicationContext/);
  assert.match(messages, /class OfferDeliveryMsg/);
  assert.match(messages, /class OfferDeliveryResultMsg/);
  assert.doesNotMatch(messages, /\bOfferDeliveryReq\b|\bOfferDeliveryRes\b/);
  assert.match(messages, /class BindCourierSessionReq \{[\s\S]*?courierId: string/);
  assert.match(messages, /type BindCourierSessionRes = \{ courierId: string \}/);
  assert.doesNotMatch(messages, /sessionRoute|BindCourierReq|BindCourierRes/);
  assert.doesNotMatch(messages, /class CourierDecisionMsg \{[^}]*attempt/);
  assert.doesNotMatch(messages, /class OfferDeliveryNotify \{[^}]*attempt/);
  assert.match(messages, /occurredAtUnixMs: number/);
  assert.match(dispatchWorker, /offerDecisionTimeout/);
  assert.match(dispatchWorker, /sweepExpiredOffers/);
  assert.match(dispatchWorker, /current\.attempt !== result\.attempt/);
  assert.match(offerHandler, /@zlinkEntrySpotActorSendHandler\(\{[\s\S]*packetName: PacketNames\.offerDelivery[\s\S]*\}\)/);
  assert.match(dispatchWorker, /this\.actors\.sendToActor\(/);
  assert.doesNotMatch(offerHandler, /class OfferDeliveryEntrySpotHandler/);
  assert.match(runSample, /Runner\/sample-runner\.mjs/);
  assert.match(sampleRunner, /dispatchEndpoint/);
  assert.match(sampleRunner, /sessionStreamEndpoint/);
  assert.match(sampleRunner, /'--config', configPath/);
  assert.doesNotMatch(clientScenario, /requestToChannel|SAMPLE_ENDPOINT|support::request_line/);
  assert.doesNotMatch(serverEntries, /--role|process\.env|courier-gateway/);
  assert.doesNotMatch(serverEntries, /SAMPLE_ENDPOINT/);
});

test('GameQuest TypeScript sample uses framework channel topology', () => {
  const clientScenario = readSample('GameQuest.Ts', 'Client/gamequest-client-scenario.ts');
  const clientMain = readSample('GameQuest.Ts', 'Client/main.ts');
  const apiModule = readSample('GameQuest.Ts', 'Server/GameApi/game-api-module.ts');
  const questModule = readSample('GameQuest.Ts', 'Server/QuestMission/gamequest-quest-module.ts');
  const apiServer = readSample('GameQuest.Ts', 'Server/GameApi/game-api-server.ts');
  const gameplayService = readSample('GameQuest.Ts', 'Server/GameApi/Application/gameplay-action-service.ts');
  const gameplayDomain = readSample('GameQuest.Ts', 'Server/GameApi/Domain/gameplay-domain.ts');
  const messageContracts = readSample('GameQuest.Ts', 'Shared/Contracts/messages.ts');
  const gameplayPublisher = readSample('GameQuest.Ts', 'Server/GameApi/Infrastructure/ZLink/gameplay-event-publisher.ts');
  const questProcessor = readSample('GameQuest.Ts', 'Server/QuestMission/Application/quest-event-processor.ts');
  const questDomain = readSample('GameQuest.Ts', 'Server/QuestMission/Domain/quest-domain.ts');
  const playerQuestProvisioner = readSample(
    'GameQuest.Ts',
    'Server/QuestMission/Infrastructure/ZLink/player-quest-spot-provisioner.ts'
  );
  const playerQuestSpot = readSample(
    'GameQuest.Ts',
    'Server/QuestMission/Infrastructure/ZLink/Spots/PlayerQuestSpot/player-quest-spot.ts'
  );
  const playerQuestSpotHandlers = readSample(
    'GameQuest.Ts',
    'Server/QuestMission/Infrastructure/ZLink/Spots/PlayerQuestSpot/player-quest-spot-handlers.ts'
  );
  const questStore = readSample('GameQuest.Ts', 'Server/Shared/Store/quest-progress-store.ts');
  const serverMain = readSample('GameQuest.Ts', 'Server/bootstrap.ts');
  const runSample = fs.readFileSync(path.join(samplesRoot, 'GameQuest.Ts', 'run_sample.sh'), 'utf8');
  const runSamplePs1 = fs.readFileSync(path.join(samplesRoot, 'GameQuest.Ts', 'run_sample.ps1'), 'utf8');
  const sampleRunner = readSample('GameQuest.Ts', 'Runner/sample-runner.mjs');

  assert.match(clientMain, /BrowserHttpClientFactory\.create\(config\.apiAHttpUrl\)/);
  assert.match(clientMain, /zlinkStreamConnectorFactory\.create/);
  assert.match(clientScenario, /apiAStream\.request\(killMonsterReq/);
  assert.match(clientScenario, /apiBReconnectStream\.request\(joinSessionReq\('player-alice'\)/);
  assert.match(clientScenario, /waitForStreamProjection\(apiBReconnectStream, 'player-alice'/);
  assert.match(clientScenario, /apiAStream\.request\(joinSessionReq/);
  assert.match(clientScenario, /waitFor<QuestCompletedNotify>/);
  assert.doesNotMatch(clientScenario, /requestToChannel|SampleNames\.questMissionRouteChannel|SAMPLE_ENDPOINT|support::request_line/);
  assert.equal((apiModule.match(/\.addRouteMesh\(/g) ?? []).length, 1);
  assert.equal((questModule.match(/\.addRouteMesh\(/g) ?? []).length, 1);
  assert.doesNotMatch(apiModule, /questMissionRouteChannel|\.channel\(/);
  assert.doesNotMatch(apiModule, /\.peerConnections\(\)/);
  assert.match(apiModule, /\.addStreamNode\(SampleNames\.playerStreamNode\)/);
  assert.match(apiServer, /http\.createServer/);
  assert.doesNotMatch(apiServer, /\/combat\/kill|\/quest\/progress/);
  assert.match(apiServer, /GameplayStateStore/);
  assert.match(apiServer, /GameQuestSelfCheckStore/);
  assert.match(gameplayService, /publishAndNotify/);
  assert.match(gameplayDomain, /monsterKilled/);
  assert.match(
    gameplayPublisher,
    /\.sendToSpot\(questMissionSpotId\(event\.playerId\), message\)[\s\S]*?\.instanceSpot\(SampleNames\.playerQuestSpotType\)[\s\S]*?\.inMesh\(SampleNames\.playerQuestSpotMesh\)/
  );
  assert.match(gameplayPublisher, /\.submit\(\)/);
  assert.match(apiModule, /zlinkFramework\(\)/);
  assert.match(apiModule, /\.listen\(config\[actorSpotEndpointKey\]\)[\s\S]*\.setRoutingIdPrefix\('gamequest-api'\)/);
  assert.match(questModule, /QuestEventProcessor/);
  assert.match(apiModule, /\.addRouteMesh\(SampleNames\.playerQuestSpotMesh\)/);
  assert.match(apiModule, /\.addEntrySpot\(GameQuestEntrySpot\)/);
  assert.match(questModule, /\.addInstanceSpotFactory\(\s*SampleNames\.playerQuestSpotType,\s*PlayerQuestSpot,/);
  assert.doesNotMatch(questModule, /questMissionInstanceChannel|addHandlerGroup\('quest-owner'\)/);
  assert.doesNotMatch(questModule, /\.add(?:Send|Request)Handler\(/);
  assert.match(questDomain, /decide\(event: GameplayEventEnvelope/);
  assert.match(messageContracts, /class JoinSessionRes \{[\s\S]*?playerId: string[\s\S]*?activeQuests/);
  assert.match(messageContracts, /class GameplayMsg \{[\s\S]*?payload: GameplayEventPayload/);
  assert.match(messageContracts, /class ClosePlayerQuestMsg/);
  assert.match(messageContracts, /type StoredQuestEvent = \{[\s\S]*?type: string[\s\S]*?payload: Record<string, unknown>/);
  assert.doesNotMatch(messageContracts, /TextEncoder|TextDecoder|decodeGameplayPayload|payload: number\[\]/);
  assert.match(playerQuestProvisioner, /\.requestToSpot\(questMissionSpotId\(playerId\), request\)/);
  assert.match(playerQuestProvisioner, /\.instanceSpot\(SampleNames\.playerQuestSpotType\)/);
  assert.match(playerQuestSpotHandlers, /implements ZLinkSpotPacketHandler<PlayerQuestSpot, GameplayMsg>/);
  assert.match(playerQuestSpotHandlers, /processor\.process\(\{[\s\S]*?payload: message\.payload[\s\S]*?\}, aggregate\)/);
  assert.match(playerQuestSpotHandlers, /processor\.rehydrate\(request\.playerId\)/);
  assert.match(playerQuestSpotHandlers, /processor\.syncProgress\(request, aggregate\)/);
  assert.match(playerQuestSpotHandlers, /store\.rebuildProjection\(request\.playerId, request\.questId, this\.events\.read\(request\.playerId\)\)/);
  assert.match(questProcessor, /syncProgress\(request: SyncQuestProgressReq, aggregate: PlayerQuestAggregate\)/);
  assert.match(playerQuestProvisioner, /questMissionSpotId\(playerId\)/);
  assert.match(playerQuestProvisioner, /ZLINK_SPOT_OUTBOUND/);
  assert.match(playerQuestSpot, /private aggregate: PlayerQuestAggregate \| undefined/);
  assert.match(playerQuestSpot, /ensureAggregate\(load: \(\) => PlayerQuestAggregate\)/);
  assert.match(playerQuestSpotHandlers, /processor\.rehydrate\(message\.playerId\)/);
  assert.match(playerQuestSpotHandlers, /spot\.replaceAggregate\(result\.aggregate\)/);
  assert.match(questProcessor, /PlayerQuestAggregate\.from\(stored\)/);
  assert.match(questStore, /recorded: boolean/);
  assert.match(questDomain, /type: 'QuestProgressed'/);
  assert.match(questDomain, /type: 'QuestCompleted'/);
  assert.match(questDomain, /type: 'QuestRewardGranted'/);
  assert.doesNotMatch(questStore, /TextEncoder|TextDecoder|encodePayload/);
  assert.match(clientScenario, /closeOwnerA\.closed \|\| closeOwnerB\.closed/);
  assert.match(serverMain, /playerQuests\.deactivate\(playerId\)/);
  assert.match(questDomain, /class PlayerQuestAggregate/);
  assert.match(questDomain, /conditionDecision/);
  assert.match(questDomain, /orderedDecision/);
  assert.match(questDomain, /QuestReconciled/);
  assert.match(clientScenario, /enter-ruins-too-early/);
  assert.match(clientScenario, /bobReconcileCompleted/);
  assert.match(questStore, /class GameplayStateStore/);
  assert.match(questStore, /class QuestEventStore/);
  assert.match(questStore, /class QuestReadModelStore/);
  assert.match(gameplayDomain, /Collected item count must be a positive integer/);
  assert.match(clientScenario, /invalid-negative-count/);
  assert.match(questDomain, /QuestStatuses/);
  assert.match(serverMain, /NestFactory\.createApplicationContext/);
  assert.match(serverMain, /createGameApiModule\(role\)/);
  assert.match(serverMain, /GAMEQUEST_SAMPLE_CONFIG/);
  assert.match(runSample, /Runner\/sample-runner\.mjs/);
  assert.match(runSamplePs1, /Runner\/sample-runner\.mjs/);
  for (const text of ['mission-a', 'mission-b', 'api-a', 'api-b', 'apiAHttpUrl', 'apiBHttpUrl']) {
    assert.match(sampleRunner, new RegExp(text));
  }
  assert.doesNotMatch(sampleRunner, /grep[^\n]*(?:GameplayMsg|QuestCompletedNotify)/);
  assert.doesNotMatch(serverMain, /SAMPLE_ENDPOINT|process\.env|--role/);
});

test('ShoppingMall TypeScript sample uses framework channel topology', () => {
  const clientScenario = readSample('ShoppingMall.Ts', 'Client/shoppingmall-client-scenario.ts');
  const clientMain = readSample('ShoppingMall.Ts', 'Client/main.ts');
  const commerceApiModule = readSample('ShoppingMall.Ts', 'Server/CommerceApi/commerce-api-module.ts');
  const commerceApiServer = readSample('ShoppingMall.Ts', 'Server/CommerceApi/commerce-api-server.ts');
  const startOrderUseCase = readSample(
    'ShoppingMall.Ts',
    'Server/CommerceApi/Application/start-order-use-case.ts'
  );
  const workflowRouter = readSample(
    'ShoppingMall.Ts',
    'Server/CommerceApi/Infrastructure/ZLink/zlink-order-workflow-router.ts'
  );
  const messageContracts = readSample('ShoppingMall.Ts', 'Shared/Contracts/messages.ts');
  const workflowService = readSample(
    'ShoppingMall.Ts',
    'Server/OrderWorkflow/Application/OrderWorkflow/order-workflow-service.ts'
  );
  const orderDomain = readSample(
    'ShoppingMall.Ts',
    'Server/OrderWorkflow/Domain/ShoppingMall/order-domain.ts'
  );
  const orderWorkflowSpot = readSample(
    'ShoppingMall.Ts',
    'Server/OrderWorkflow/Infrastructure/ZLink/Spots/OrderWorkflowSpot/order-workflow-spot.ts'
  );
  const startOrderSpotHandler = readSample(
    'ShoppingMall.Ts',
    'Server/OrderWorkflow/Infrastructure/ZLink/Spots/OrderWorkflowSpot/Handlers/start-order-workflow-handler.ts'
  );
  const orderEvents = readSample('ShoppingMall.Ts', 'Server/Shared/Domain/order-events.ts');
  const workflowModule = readSample('ShoppingMall.Ts', 'Server/OrderWorkflow/shoppingmall-workflow-module.ts');
  const orderStore = readSample('ShoppingMall.Ts', 'Server/Shared/Store/order-store.ts');
  const serverMain = readSample('ShoppingMall.Ts', 'Server/bootstrap.ts');
  const runSample = fs.readFileSync(path.join(samplesRoot, 'ShoppingMall.Ts', 'run_sample.sh'), 'utf8');
  const runSamplePs1 = fs.readFileSync(path.join(samplesRoot, 'ShoppingMall.Ts', 'run_sample.ps1'), 'utf8');
  const sampleRunner = readSample('ShoppingMall.Ts', 'Runner/sample-runner.mjs');

  assert.match(clientMain, /ZLinkHttpClient\.create\(config\.apiAHttpUrl\)/);
  assert.match(clientMain, /ZLinkHttpClient\.create\(config\.apiBHttpUrl\)/);
  assert.match(clientScenario, /\.post\('\/orders\/start'\)/);
  assert.match(clientScenario, /\.get\(`\/orders\/\$\{orderId\}`\)/);
  assert.match(clientScenario, /shoppingmall-payment-failure=completed/);
  assert.match(clientScenario, /\.post\('\/self-check\/assert'\)/);
  assert.doesNotMatch(clientScenario, /requestToChannel|SampleNames\.orderWorkflowRouteChannel|SAMPLE_ENDPOINT|support::request_line/);
  assert.match(commerceApiModule, /zlinkFramework\(\)/);
  assert.match(commerceApiModule, /\.addRouteMesh\(SampleNames\.orderWorkflowSpotMesh\)/);
  assert.match(commerceApiModule, /\.listen\('tcp:\/\/127\.0\.0\.1:0'\)/);
  assert.equal((commerceApiModule.match(/\.addRouteMesh\(/g) ?? []).length, 1);
  for (const file of sampleSourceFiles(path.join(samplesRoot, 'ShoppingMall.Ts', 'Server', 'CommerceApi'))) {
    assert.doesNotMatch(fs.readFileSync(file, 'utf8'), /OrderWorkflow\//);
  }
  assert.match(commerceApiServer, /http\.createServer/);
  assert.match(commerceApiServer, /\/orders\/start/);
  assert.match(commerceApiServer, /StartOrderUseCase/);
  assert.match(startOrderUseCase, /store\.reserveOrder\(request\)/);
  assert.doesNotMatch(startOrderUseCase, /PacketNames|\.packetName\(/);
  assert.match(workflowRouter, /private request<TResponse>/);
  assert.match(workflowRouter, /start\(request: StartOrderWorkflowReq\)/);
  assert.doesNotMatch(workflowRouter, /\.packetName\(/);
  assert.match(messageContracts, /@ZLinkPacket\(PacketNames\.startOrderWorkflowReq\)/);
  assert.match(messageContracts, /class StartOrderWorkflowReq[\s\S]*?sourceCommandId: string/);
  assert.match(messageContracts, /interface StartOrderRes \{[\s\S]*?state: OrderState/);
  assert.match(messageContracts, /class ContinueOrderWorkflowReq[\s\S]*?sourceCommandId: string/);
  assert.match(messageContracts, /class RebuildOrderProjectionReq[\s\S]*?sourceCommandId: string/);
  assert.doesNotMatch(messageContracts, /PrepareInventory|VerifyExpectedVersion/);
  assert.match(
    workflowRouter,
    /requestToSpot\(orderId, payload\)[\s\S]*?\.instanceSpot\(SampleNames\.orderWorkflowSpotType\)[\s\S]*?\.inMesh\(SampleNames\.orderWorkflowSpotMesh\)/
  );
  assert.match(workflowModule, /zlinkFramework\(\)/);
  assert.equal((workflowModule.match(/\.addRouteMesh\(/g) ?? []).length, 1);
  assert.doesNotMatch(workflowModule, /workflowChannelEndpointForRole/);
  assert.match(workflowModule, /\.addRouteMesh\(SampleNames\.orderWorkflowSpotMesh\)/);
  assert.match(workflowModule, /\.addInstanceSpotFactory\(\s*SampleNames\.orderWorkflowSpotType,\s*OrderWorkflowSpot,/);
  assert.match(workflowModule, /OrderWorkflowService/);
  assert.doesNotMatch(workflowModule, /orderWorkflowChannel|addHandlerGroup\('workflow'\)/);
  assert.match(orderWorkflowSpot, /class OrderWorkflowSpot implements ZLinkInstanceSpot/);
  assert.match(startOrderSpotHandler, /ZLinkSpotRequestHandler<OrderWorkflowSpot/);
  assert.doesNotMatch(orderWorkflowSpot, /context\.handlers\.add/);
  assert.match(startOrderSpotHandler, /@zlinkSpotPacketHandler\(\{ spot: \(\) => OrderWorkflowSpot/);
  assert.match(workflowService, /start\(request: StartOrderWorkflowReq/);
  assert.match(workflowService, /continue\(request: \{ orderId: string \}/);
  assert.match(orderDomain, /class OrderAggregate/);
  assert.match(orderEvents, /interface StoredOrderEvent/);
  assert.match(orderEvents, /payload: readonly number\[\]/);
  assert.match(orderStore, /constructor\(workDir: string\)/);
  assert.match(orderStore, /class ExpectedVersionConflict/);
  assert.match(orderStore, /interrupted after inventory effect/);
  assert.match(orderStore, /overlap writer rejected/);
  assert.match(orderStore, /payload: \[\.\.\.Buffer\.from/);
  assert.match(serverMain, /NestFactory\.createApplicationContext/);
  assert.match(serverMain, /SampleNames\.workflowA/);
  assert.match(serverMain, /SampleNames\.apiB/);
  assert.match(runSample, /Runner\/sample-runner\.mjs/);
  assert.match(runSamplePs1, /Runner\/sample-runner\.mjs/);
  for (const text of ['workflow-a', 'workflow-b', 'api-a', 'api-b', 'apiAHttpUrl', 'apiBHttpUrl']) {
    assert.match(sampleRunner, new RegExp(text));
  }
  assert.match(commerceApiModule, /config\.logDir/);
  assert.match(workflowModule, /config\.logDir/);
  assert.match(sampleRunner, /logDir: ctx\.logDir/);
  assert.doesNotMatch(serverMain, /SAMPLE_ENDPOINT|process\.env|--role/);
});

test('common-spec TypeScript clients do not import server modules', () => {
  const violations = [];
  for (const sample of ['DeliveryDispatch.Ts', 'GameQuest.Ts', 'ShoppingMall.Ts']) {
    for (const file of sampleSourceFiles(path.join(samplesRoot, sample, 'Client'))) {
      const content = fs.readFileSync(file, 'utf8');
      if (/from ['"]\.\.\/Server\//.test(content)) {
        violations.push(relativePath(samplesRoot, file));
      }
    }
  }

  assert.deepEqual(violations, []);
});

test('node samples use only framework and connector public APIs', () => {
  const violations = [];
  for (const file of sampleSourceFiles(samplesRoot)) {
    const content = fs.readFileSync(file, 'utf8');
    if (/bindings\/node|runtime\/native|src\/zlink\/runtime|packages\/[^/]+\/src/.test(content)) {
      violations.push(relativePath(workspaceRoot, file));
    }
  }

  assert.deepEqual(violations, []);
});

test('node framework samples exercise the real NestJS application context', () => {
  const missing = [];
  for (const sample of ['TicTacToe.Ts', 'Bingo.Ts']) {
    const usesNestModule = sampleSourceFiles(path.join(samplesRoot, sample))
      .some((file) => {
        const content = fs.readFileSync(file, 'utf8');
        return content.includes('@zlink-systems/nestjs')
          || content.includes('packages/nestjs/dist');
      });
    if (!usesNestModule) {
      missing.push(sample);
    }
  }

  const hiddenServerRuntime = [];
  for (const file of sampleSourceFiles(samplesRoot)) {
    const relative = relativePath(samplesRoot, file);
    if (relative.startsWith('shared/') || relative.includes('/dist/')) {
      continue;
    }
    const content = fs.readFileSync(file, 'utf8');
    if (/startChannelServer|startRouteServer|createZLinkNestRuntime|nestjs-provider-runtime/.test(content)) {
      hiddenServerRuntime.push(relative);
    }
  }

  const serverRoles = [
    ['TicTacToe.Ts/Server/Api/main.ts', 'TicTacToe.Ts/Server/Api/tictactoe-api-module.ts', 'createTicTacToeApiModule'],
    ['TicTacToe.Ts/Server/Play/main.ts', 'TicTacToe.Ts/Server/Play/tictactoe-play-module.ts', 'createTicTacToePlayModule'],
    ['Bingo.Ts/Server/Api/main.ts', 'Bingo.Ts/Server/Api/bingo-api-module.ts', 'createBingoApiModule'],
    ['Bingo.Ts/Server/Play/main.ts', 'Bingo.Ts/Server/Play/bingo-play-module.ts', 'createBingoPlayModule'],
    ['Bingo.Ts/Server/Session/main.ts', 'Bingo.Ts/Server/Session/bingo-session-module.ts', 'createBingoSessionModule']
  ];
  for (const [mainRelative, moduleRelative, factoryName] of serverRoles) {
    const main = fs.readFileSync(path.join(samplesRoot, mainRelative), 'utf8');
    const module = fs.readFileSync(path.join(samplesRoot, moduleRelative), 'utf8');
    if (!main.includes(factoryName)) {
      missing.push(`${mainRelative}:${factoryName}`);
    }
    if (!main.includes('NestFactory.createApplicationContext')) {
      missing.push(`${mainRelative}:NestFactory.createApplicationContext`);
    }
    for (const text of ['providers: [', "require('@nestjs/common')", 'ZLinkModule.forRoot']) {
      if (main.includes(text)) {
        hiddenServerRuntime.push(`${mainRelative}:${text}`);
      }
    }
    if (
      !module.includes("require('@nestjs/common')")
      && !module.includes("from '@nestjs/common'")
      && !module.includes('zlinkModule')
    ) {
      missing.push(`${moduleRelative}:@nestjs/common`);
    }
    if (!module.includes('ZLinkModule.forRoot')) {
      missing.push(`${moduleRelative}:ZLinkModule.forRoot`);
    }
    if (
      !moduleRelative.includes('/Registry/')
      && !/providers:\s*(?:\[|zlinkDiscoverProviders)/.test(module)
      && !module.includes('providerDiscovery')
      && !module.includes('zlinkModule(__dirname')
    ) {
      missing.push(`${moduleRelative}:providers`);
    }
  }

  assert.deepEqual(missing, []);
  assert.deepEqual(hiddenServerRuntime, []);
});

test('TicTacToe TypeScript sample builds and exposes basic TypeScript roles', () => {
  const packageJson = fs.readFileSync(path.join(samplesRoot, 'TicTacToe.Ts', 'package.json'), 'utf8');
  const tsconfig = fs.readFileSync(path.join(samplesRoot, 'TicTacToe.Ts', 'tsconfig.json'), 'utf8');
  const client = fs.readFileSync(path.join(samplesRoot, 'TicTacToe.Ts', 'Client', 'main.ts'), 'utf8');
  const api = fs.readFileSync(path.join(samplesRoot, 'TicTacToe.Ts', 'Server', 'Api', 'main.ts'), 'utf8');
  const play = fs.readFileSync(path.join(samplesRoot, 'TicTacToe.Ts', 'Server', 'Play', 'main.ts'), 'utf8');
  const runSamples = fs.readFileSync(path.join(samplesRoot, 'run_samples.sh'), 'utf8');
  const required = [
    [packageJson, '@zlink-systems/sample-tictactoe-ts'],
    [packageJson, 'tsc -p tsconfig.json'],
    [tsconfig, '"outDir": "dist"'],
    [tsconfig, '"Server/**/*.ts"'],
    [client, 'loadSampleConfig'],
    [client, 'PASS TicTacToe.Ts'],
    [api, 'TicTacToeApiModule'],
    [play, 'TicTacToePlayModule'],
    [runSamples, 'TicTacToe.Ts'],
    [runSamples, 'runner="${SCRIPT_DIR}/${sample}/run_sample.sh"']
  ];
  const missing = required
    .filter(([content, text]) => !content.includes(text))
    .map(([, text]) => text);
  const violations = [];
  for (const file of sampleSourceFiles(path.join(samplesRoot, 'TicTacToe.Ts'))) {
    if (!file.endsWith('.ts')) {
      continue;
    }
    const content = fs.readFileSync(file, 'utf8');
    if (/require\(['"][^'"]*samples\/TicTacToe\/|from ['"][^'"]*samples\/TicTacToe\//.test(content)) {
      violations.push(`${relativePath(samplesRoot, file)} references the JavaScript TicTacToe sample`);
    }
    if (content.includes('@ts-nocheck')) {
      violations.push(`${relativePath(samplesRoot, file)} disables TypeScript checking`);
    }
  }

  assert.deepEqual(missing, []);
  assert.deepEqual(violations, []);
});

test('TicTacToe TypeScript sample implements the common game state contract', () => {
  const client = fs.readFileSync(path.join(samplesRoot, 'TicTacToe.Ts', 'Client', 'tictactoe-client-scenario.ts'), 'utf8');
  const board = fs.readFileSync(path.join(samplesRoot, 'TicTacToe.Ts', 'Server', 'Play', 'Domain', 'TicTacToe', 'tictactoe-board.ts'), 'utf8');
  const match = fs.readFileSync(path.join(samplesRoot, 'TicTacToe.Ts', 'Server', 'Play', 'Domain', 'TicTacToe', 'tictactoe-match.ts'), 'utf8');
  const joinHandler = fs.readFileSync(path.join(samplesRoot, 'TicTacToe.Ts', 'Server', 'Play', 'Infrastructure', 'ZLink', 'Spots', 'EntrySpot', 'Handlers', 'play-actor-join-game-handler.ts'), 'utf8');
  const moveHandler = fs.readFileSync(path.join(samplesRoot, 'TicTacToe.Ts', 'Server', 'Play', 'Infrastructure', 'ZLink', 'Spots', 'TicTacToeGameSpot', 'Handlers', 'play-actor-place-mark-handler.ts'), 'utf8');
  const gameSpot = fs.readFileSync(path.join(samplesRoot, 'TicTacToe.Ts', 'Server', 'Play', 'Infrastructure', 'ZLink', 'Spots', 'TicTacToeGameSpot', 'tictactoe-game-spot.ts'), 'utf8');
  const playActor = fs.readFileSync(path.join(samplesRoot, 'TicTacToe.Ts', 'Server', 'Play', 'Infrastructure', 'ZLink', 'Actors', 'play-actor.ts'), 'utf8');
  const playSession = fs.readFileSync(path.join(samplesRoot, 'TicTacToe.Ts', 'Server', 'Play', 'Infrastructure', 'ZLink', 'Sessions', 'play-session.ts'), 'utf8');
  const authenticateHandler = fs.readFileSync(path.join(samplesRoot, 'TicTacToe.Ts', 'Server', 'Play', 'Infrastructure', 'ZLink', 'Sessions', 'Handlers', 'authenticate-play-session-handler.ts'), 'utf8');
  const required = [
    [board, 'class TicTacToeBoard'],
    [match, 'class TicTacToeMatch'],
    [match, 'this.status = GameStatus.InProgress'],
    [match, 'this.status = GameStatus.Won'],
    [match, 'this.status = GameStatus.TurnTimedOut'],
    [joinHandler, '.joinSpot(request.roomId, joinRequest)'],
    [moveHandler, 'spot.placeMark(actor.actorId, request.cell)'],
    [gameSpot, 'gameStateNotify(state)'],
    [playActor, 'this.context.boundSession'],
    [authenticateHandler, 'context.actors.bindOrGet(actorRef)'],
    [client, 'payload.state.status === GameStatus.InProgress'],
    [client, 'stateOf(client1FinalMove).status === GameStatus.Won']
  ];
  const missing = required
    .filter(([content, text]) => !content.includes(text))
    .map(([, text]) => text);
  const violations = [];
  for (const [content, text] of [
    [match, "this.status = 'Running'"],
    [match, "this.status = 'Finished'"],
    [client, "status === 'Running'"],
    [client, "status, 'Finished'"],
    [gameSpot, 'bindGameActions('],
    [gameSpot, 'Map<string, PlaySpotActor>']
  ]) {
    if (content.includes(text)) {
      violations.push(text);
    }
  }

  assert.deepEqual(missing, []);
  assert.deepEqual(violations, []);
});

test('Bingo TypeScript sample builds and exposes separated TypeScript roles', () => {
  const packageJson = fs.readFileSync(path.join(samplesRoot, 'Bingo.Ts', 'package.json'), 'utf8');
  const tsconfig = fs.readFileSync(path.join(samplesRoot, 'Bingo.Ts', 'tsconfig.json'), 'utf8');
  const client = fs.readFileSync(path.join(samplesRoot, 'Bingo.Ts', 'Client', 'main.ts'), 'utf8');
  const api = fs.readFileSync(path.join(samplesRoot, 'Bingo.Ts', 'Server', 'Api', 'main.ts'), 'utf8');
  const session = fs.readFileSync(path.join(samplesRoot, 'Bingo.Ts', 'Server', 'Session', 'main.ts'), 'utf8');
  const play = fs.readFileSync(path.join(samplesRoot, 'Bingo.Ts', 'Server', 'Play', 'main.ts'), 'utf8');
  const runSamples = fs.readFileSync(path.join(samplesRoot, 'run_samples.sh'), 'utf8');
  const required = [
    [packageJson, '@zlink-systems/sample-bingo-ts'],
    [packageJson, 'tsc -p tsconfig.json'],
    [tsconfig, '"outDir": "dist"'],
    [tsconfig, '"Server/**/*.ts"'],
    [client, "from './bingo-client-scenario'"],
    [client, 'loadSampleConfig'],
    [client, 'bingo=completed'],
    [api, 'async function bootstrap'],
    [session, 'async function bootstrap'],
    [play, 'async function bootstrap'],
    [runSamples, 'Bingo.Ts'],
    [runSamples, 'runner="${SCRIPT_DIR}/${sample}/run_sample.sh"']
  ];
  const missing = required
    .filter(([content, text]) => !content.includes(text))
    .map(([, text]) => text);
  const violations = [];
  for (const file of sampleSourceFiles(path.join(samplesRoot, 'Bingo.Ts'))) {
    if (!file.endsWith('.ts')) {
      continue;
    }
    const content = fs.readFileSync(file, 'utf8');
    if (/require\(['"][^'"]*samples\/Bingo\/|from ['"][^'"]*samples\/Bingo\//.test(content)) {
      violations.push(`${relativePath(samplesRoot, file)} references the JavaScript Bingo sample`);
    }
    if (content.includes('@ts-nocheck')) {
      violations.push(`${relativePath(samplesRoot, file)} disables TypeScript checking`);
    }
  }

  assert.deepEqual(missing, []);
  assert.deepEqual(violations, []);
});

test('Bingo TypeScript sample uses channel peers and location store registration where supported', () => {
  const api = fs.readFileSync(path.join(samplesRoot, 'Bingo.Ts', 'Server', 'Api', 'main.ts'), 'utf8');
  const apiModule = fs.readFileSync(path.join(samplesRoot, 'Bingo.Ts', 'Server', 'Api', 'bingo-api-module.ts'), 'utf8');
  const play = fs.readFileSync(path.join(samplesRoot, 'Bingo.Ts', 'Server', 'Play', 'main.ts'), 'utf8');
  const playModule = fs.readFileSync(path.join(samplesRoot, 'Bingo.Ts', 'Server', 'Play', 'bingo-play-module.ts'), 'utf8');
  const session = fs.readFileSync(path.join(samplesRoot, 'Bingo.Ts', 'Server', 'Session', 'main.ts'), 'utf8');
  const sessionModule = fs.readFileSync(path.join(samplesRoot, 'Bingo.Ts', 'Server', 'Session', 'bingo-session-module.ts'), 'utf8');
  const locationStore = fs.readFileSync(path.join(samplesRoot, 'Bingo.Ts', 'Server', 'Configuration', 'location-store.ts'), 'utf8');
  const required = [
    [locationStore, 'ZLinkRedisLocationStore'],
    [locationStore, 'redisEndpoint'],
    [locationStore, 'redisKeyPrefix'],
    [apiModule, '.addLocationStore(createBingoLocationStore(config))'],
    [apiModule, 'bingoLocationOptions(builder.configureLocations())'],
    [apiModule, '.addRouteMesh(SampleNames.playMeshName'],
    [apiModule, '.addRouteMesh(SampleNames.matchmakingMeshName'],
    [apiModule, '.listen(config.apiEndpoint)'],
    [playModule, '.addLocationStore(createBingoLocationStore(config))'],
    [playModule, "createBingoRelocationStore(config)"],
    [playModule, '.addRelocationStore(createBingoRelocationStore(config))'],
    [playModule, 'bingoLocationOptions(builder.configureLocations())'],
    [playModule, '.addRouteMesh(SampleNames.roomSpotNode'],
    [playModule, '.listen(config.playSpotEndpoint)'],
    [sessionModule, '.addLocationStore(createBingoLocationStore(endpoints))'],
    [sessionModule, 'createBingoRelocationStore(endpoints)'],
    [sessionModule, '.addRelocationStore(createBingoRelocationStore(endpoints))'],
    [sessionModule, 'bingoLocationOptions(builder.configureLocations())']
  ];
  const missing = required
    .filter(([content, text]) => !content.includes(text))
    .map(([, text]) => text);
  const violations = [];
  for (const [content, text] of [
    [api, 'createRegistryClient'],
    [play, 'createRegistryClient'],
    [session, 'createRegistryClient'],
    [api, 'registry.resolve'],
    [play, 'registry.register'],
    [session, 'registry.resolve'],
    [apiModule, '.useDiscovery()'],
    [apiModule, '.addRegistryEndpoint('],
    [playModule, '.useDiscovery()'],
    [playModule, '.addRegistryEndpoint('],
    [sessionModule, '.useDiscovery()'],
    [sessionModule, '.addRegistryEndpoint('],
    [session, 'process.env.BINGO_API_ENDPOINT'],
    [session, 'process.env.BINGO_PLAY_ENDPOINT'],
    [api, 'process.env.BINGO_PLAY_ENDPOINT']
  ]) {
    if (content.includes(text)) {
      violations.push(text);
    }
  }

  assert.deepEqual(missing, []);
  assert.deepEqual(violations, []);
  assert.equal((apiModule.match(/\.addRouteMesh\(/g) ?? []).length, 2);
  assert.equal((playModule.match(/\.addRouteMesh\(/g) ?? []).length, 1);
  assert.equal((sessionModule.match(/\.addRouteMesh\(/g) ?? []).length, 1);
});

test('Bingo TypeScript sample publishes drawn number before finished notify', () => {
  const roomSpot = fs.readFileSync(path.join(
    samplesRoot,
    'Bingo.Ts',
    'Server',
    'Play',
    'Infrastructure',
    'ZLink',
    'Spots',
    'BingoRoomSpot',
    'bingo-room-spot.ts'
  ), 'utf8');
  const drawIndex = roomSpot.indexOf('new BingoNumberDrawnNotify(');
  const finishedBranchIndex = roomSpot.indexOf('if (drawn.finished)');
  const endedIndex = roomSpot.indexOf('new BingoGameEndedNotify(');

  assert.equal(drawIndex > 0, true);
  assert.equal(finishedBranchIndex > drawIndex, true);
  assert.equal(endedIndex > finishedBranchIndex, true);
});

test('node topology samples run server roles as separate processes over TCP route endpoints', () => {
  const cases = [
    {
      sample: 'TicTacToe.Ts',
      serverEntries: ['Server/Api/main.ts', 'Server/Play/main.ts'],
      processes: ['dist/Server/Api/main.js', 'dist/Server/Play/main.js']
    },
    {
      sample: 'Bingo.Ts',
      serverEntries: ['Server/Api/main.ts', 'Server/Play/main.ts', 'Server/Session/main.ts'],
      processes: ['dist/Server/Api/main.js', 'dist/Server/Play/main.js', 'dist/Server/Session/main.js']
    }
  ];
  for (const { sample, serverEntries, processes } of cases) {
    const runSample = fs.readFileSync(path.join(samplesRoot, sample, 'Runner', 'sample-runner.mjs'), 'utf8');
    for (const serverRelative of serverEntries) {
      const serverContent = fs.readFileSync(path.join(samplesRoot, sample, serverRelative), 'utf8');
      assert.match(serverContent, /SAMPLE_CONFIG|forRootFactory|create[A-Za-z]+Module/);
    }
    for (const processEntry of processes) {
      assert.equal(runSample.includes(processEntry), true, `${sample} runner must start ${processEntry}`);
    }
    assert.match(runSample, /tcp:\/\/127\.0\.0\.1:\$\{await ctx\.port\(\)\}/);
    assert.match(runSample, /writeConfig\(/);
    assert.match(runSample, /'--config'/);
    assert.match(runSample, /ctx\.start\(/);
  }
});

test('node topology samples do not use stdin command protocol as messaging', () => {
  const violations = [];
  for (const file of sampleSourceFiles(samplesRoot)) {
    const content = fs.readFileSync(file, 'utf8');
    if (/runRoleServer|startRoleProcess|withRoleProcess|command ===|stdin\.write/.test(content)) {
      violations.push(relativePath(samplesRoot, file));
    }
  }

  assert.deepEqual(violations, []);
});

test('node samples do not hide readiness with sleeps or pre-ready pings', () => {
  const violations = [];
  const allowedTimingFiles = new Set([
    'samples/run-sample.mjs',
    'samples/Bingo.Ts/run_sample.ps1',
    'samples/Bingo.Ts/run_sample.sh',
    'samples/DeliveryDispatch.Ts/Client/deliverydispatch-client-scenario.ts',
    'samples/DeliveryDispatch.Ts/Server/DispatchCenter/dispatch-worker.ts',
    'samples/GameQuest.Ts/Client/gamequest-client-scenario.ts',
    'samples/GameQuest.Ts/Server/GameApi/gamequest-session.ts',
    'samples/GameQuest.Ts/Server/GameApi/Infrastructure/ZLink/gameplay-event-publisher.ts',
    'samples/SupportChat.Ts/Client/supportchat-client-scenario.ts',
    'samples/SupportChat.Ts/Server/Probe/main.ts',
    'samples/SupportChat.Ts/Server/Support/notification-delivery-log.ts',
    'samples/SupportChat.Ts/Server/runtime-support.ts',
    'samples/ShoppingMall.Ts/Client/shoppingmall-client-scenario.ts',
    'samples/ShoppingMall.Ts/Server/CommerceApi/Infrastructure/ZLink/zlink-order-workflow-router.ts'
  ]);
  for (const file of sampleSourceFiles(samplesRoot)) {
    if (allowedTimingFiles.has(relativePath(workspaceRoot, file))) {
      continue;
    }
    const content = fs.readFileSync(file, 'utf8');
    if (/\bsleep\s*\(|setTimeout\s*\(|beforeReady/.test(content)) {
      violations.push(relativePath(workspaceRoot, file));
    }
  }

  assert.deepEqual(violations, []);
});

test('node top-level sample runner only invokes selected samples in order', () => {
  const runSamples = fs.readFileSync(path.join(samplesRoot, 'run_samples.sh'), 'utf8');
  assert.match(runSamples, /for sample in "\$\{samples\[@\]\}"/);
  assert.match(runSamples, /"\$\{runner\}"/);
  assert.doesNotMatch(runSamples, /node --test|retry|sleep|grep/);
});

test('node RegistryMessaging e2e endpoints do not hide local routing failures with retry loops', () => {
  const registryMessagingRoot = path.join(workspaceRoot, 'e2e', 'RegistryMessaging');
  const endpointFiles = [
    'Server/Provider/Endpoints/provider-endpoints.ts',
    'Server/Consumer/Endpoints/consumer-endpoints.ts',
    'Server/Workflow/Endpoints/workflow-endpoints.ts'
  ];
  const violations = [];
  for (const relative of endpointFiles) {
    const content = fs.readFileSync(path.join(registryMessagingRoot, relative), 'utf8');
    for (const pattern of [
      /\bWithRetry\b/,
      /\bretryUntil\b/,
      /Timed out waiting for .*route/
    ]) {
      if (pattern.test(content)) {
        violations.push(`${relative}:${pattern.source}`);
      }
    }
  }

  assert.deepEqual(violations, []);
});

test('node client samples wait for push packets through stream connector helpers', () => {
  const bingoApp = fs.readFileSync(path.join(samplesRoot, 'Bingo.Ts', 'Client', 'bingo-client-scenario.ts'), 'utf8');
  const ticTacToeClient = fs.readFileSync(path.join(samplesRoot, 'TicTacToe.Ts', 'Client', 'tictactoe-client-scenario.ts'), 'utf8');
  const missing = [];
  const violations = [];
  for (const [name, content] of [
    ['Bingo.Ts/Client/bingo-client-scenario.ts', bingoApp],
    ['TicTacToe.Ts/Client/tictactoe-client-scenario.ts', ticTacToeClient]
  ]) {
    if (!/\.waitFor(?:<|\()/.test(content)) {
      missing.push(`${name}:.waitFor(`);
    }
    if (!/\.waitFor(?:<|\()[\s\S]*?\.submit\(/.test(content)) {
      missing.push(`${name}:.waitFor(...).submit(`);
    }
  }
  for (const [name, content] of [
    ['Bingo.Ts/Client/bingo-client-scenario.ts', bingoApp],
    ['TicTacToe.Ts/Client/tictactoe-client-scenario.ts', ticTacToeClient]
  ]) {
    if (/waitForJson|waitForNotify|async function waitFor\s*\(|\.waitFor(?:<[^>]+>)?\([^)]*,/.test(content)) {
      violations.push(name);
    }
    if (/\.on<[^>]+>\(/.test(content)) {
      violations.push(`${name}:.on`);
    }
  }

  assert.deepEqual(missing, []);
  assert.deepEqual(violations, []);
});

test('node client scenarios follow the common sample document order', () => {
  const bingoApp = fs.readFileSync(path.join(samplesRoot, 'Bingo.Ts', 'Client', 'bingo-client-scenario.ts'), 'utf8');
  const ticTacToeClient = fs.readFileSync(path.join(samplesRoot, 'TicTacToe.Ts', 'Client', 'tictactoe-client-scenario.ts'), 'utf8');

  assertOrdered('Bingo.Ts/Client/bingo-client-scenario.ts', bingoApp, [
    "1. Clients connect only to Session streams, authenticate",
    'client1.request(new AuthenticateReq({ accessToken: BingoSamplePlayers.player1 }))',
    'client2.request(new AuthenticateReq({ accessToken: BingoSamplePlayers.player2 }))',
    '2. player-1 matches first',
    'const [client1MatchRes] = await Promise.all([',
    "client1.request(new MatchBingoReq({ mode: 'two-player' }))",
    'client1.expectNone<PlayerJoinedNotify>',
    'client1MatchRes.roomId.length > 0',
    '4-6. player-2 joins the same room',
    '.waitFor<PlayerJoinedNotify>',
    '.waitFor<StateEnvelope>(PacketNames.gameStartedNotify)',
    '.waitFor<StateEnvelope>(PacketNames.gameStartedNotify)',
    'const [client2MatchRes] = await Promise.all([',
    "client2.request(new MatchBingoReq({ mode: 'two-player' }))",
    'client2.expectNone<PlayerJoinedNotify>',
    '7. Both clients submit deterministic cards',
    '.request(new SubmitBingoCardReq',
    '.request(new SubmitBingoCardReq',
    'stateOf(client1Card).players.length === 2',
    '8. Number drawing is server-driven',
    'requireSameDraw(client1Draw.payload, client2Draw.payload, drawTask.drawSeq)',
    '9. Both clients receive the final finished state',
    'client1EndedTask',
    'ended.status === BingoRoomStatus.Finished'
  ]);

  assertOrdered('TicTacToe.Ts/Client/tictactoe-client-scenario.ts', ticTacToeClient, [
    '1. Create the room through API',
    ".body(createGameHttpReq('match-ready'))",
    'game.roomId.length > 0',
    'const hostPlayEndpoint = game.playEndpoints[0]',
    'createPlayerClient(hostPlayEndpoint',
    'createPlayerClient(observerPlayEndpoint',
    '2. Host, guest, and observer connect directly',
    "client1.request(authenticateReq('player-x'))",
    "client2.request(authenticateReq('player-o'))",
    '3. Host joins by explicit RoomId',
    'client1.request(joinGameReq(game.roomId))',
    "stateOf(client1Join).roomId === game.roomId",
    'client1State.payload.state.xActorId === client1Auth.player.actorId',
    'client1SawClient2Join',
    '4-6. Guest joins by the same RoomId',
    'client2.request(joinGameReq(game.roomId))',
    'client2State.payload.state.oActorId === client2Auth.player.actorId',
    'client1Running.payload.state.nextTurn === GameMarks.x',
    '7. Each move response is matched with the opponent notify',
    'client1.request(placeMarkStreamReq(0))',
    "stateOf(client1Move1).board === 'X........'",
    '8. The final host move wins',
    'client1.request(placeMarkStreamReq(2))',
    "stateOf(client1FinalMove).board === 'XXXOO....'",
    'stateOf(client1FinalMove).status === GameStatus.Won'
  ]);
});

test('node samples use the codecs required by the common specs', () => {
  const ticTacToeClient = fs.readFileSync(path.join(samplesRoot, 'TicTacToe.Ts', 'Client', 'tictactoe-client-scenario.ts'), 'utf8');
  const ticTacToePlay = fs.readFileSync(path.join(samplesRoot, 'TicTacToe.Ts', 'Server', 'Play', 'tictactoe-play-module.ts'), 'utf8');
  const ticTacToeContracts = fs.readFileSync(path.join(samplesRoot, 'TicTacToe.Ts', 'Shared', 'Contracts', 'messages.ts'), 'utf8');
  const bingoClient = fs.readFileSync(path.join(samplesRoot, 'Bingo.Ts', 'Client', 'main.ts'), 'utf8');
  const bingoSessionModule = fs.readFileSync(path.join(samplesRoot, 'Bingo.Ts', 'Server', 'Session', 'bingo-session-module.ts'), 'utf8');
  const bingoAuthenticateHandler = fs.readFileSync(path.join(samplesRoot, 'Bingo.Ts', 'Server', 'Session', 'Sessions', 'Handlers', 'authenticate-session-handler.ts'), 'utf8');
  const bingoRoomSpot = fs.readFileSync(path.join(samplesRoot, 'Bingo.Ts', 'Server', 'Play', 'Infrastructure', 'ZLink', 'Spots', 'BingoRoomSpot', 'bingo-room-spot.ts'), 'utf8');
  const bingoContracts = fs.readFileSync(path.join(samplesRoot, 'Bingo.Ts', 'Shared', 'Contracts', 'messages.ts'), 'utf8');
  const bingoCodec = fs.readFileSync(path.join(samplesRoot, 'Bingo.Ts', 'Shared', 'Contracts', 'protobuf-codec.ts'), 'utf8');
  const bingoBrowserCodec = fs.readFileSync(path.join(samplesRoot, 'Bingo.Ts', 'Shared', 'Contracts', 'protobuf-browser-codec.ts'), 'utf8');
  const bingoFrameworkCodec = fs.readFileSync(path.join(samplesRoot, 'Bingo.Ts', 'Shared', 'Contracts', 'protobuf-framework-codec.ts'), 'utf8');
  const bingoProto = fs.readFileSync(path.join(samplesRoot, 'Bingo.Ts', 'Shared', 'Contracts', 'bingo_messages.proto'), 'utf8');
  const required = [
    [ticTacToeClient, 'zlinkStreamConnectorFactory.create'],
    [ticTacToePlay, '.addStreamNode(SampleNames.playStream'],
    [bingoClient, 'bingoProtobuf'],
    [bingoSessionModule, '.use(bingoFrameworkProtobuf)'],
    [bingoSessionModule, '.codecs()'],
    [bingoBrowserCodec, 'createZlinkStreamProtobufEnvelopeCodec'],
    [bingoFrameworkCodec, 'createZlinkProtobufEnvelopeCodec'],
    [bingoCodec, 'BingoGeneratedProtobufCodec.encode'],
    [bingoAuthenticateHandler, 'payload.decode<AuthenticateReq>'],
    [bingoRoomSpot, 'request.decode<BingoRoomJoinReq>'],
    [bingoProto, 'message AuthenticateReq'],
    [bingoProto, 'message BingoRoomState'],
    [bingoProto, 'message BingoNumberDrawnNotify']
  ];
  const missing = required
    .filter(([content, text]) => !content.includes(text))
    .map(([, text]) => text);
  const violations = [];
  for (const sample of ['Bingo.Ts', 'TicTacToe.Ts']) {
    for (const file of sampleSourceFiles(path.join(samplesRoot, sample))) {
      if (!file.endsWith('.ts')) {
        continue;
      }
      const content = fs.readFileSync(file, 'utf8');
      const relative = relativePath(samplesRoot, file);
      if (sample === 'TicTacToe.Ts' && /MessagePack|msgpack|toMsgPack|fromMsgPack|zlinkStreamMessagePackCodec|createMessagePackMessage|readMessagePackMessage/.test(content)) {
        violations.push(relative);
      }
      if (sample === 'Bingo.Ts' && /MessagePack|msgpack|toMsgPack|fromMsgPack|zlinkStreamMessagePackCodec|createMessagePackMessage|readMessagePackMessage/.test(content)) {
        violations.push(relative);
      }
      if (/bingoChannelHandlerOptions|decodeBingoChannelReply|submit<Buffer>|\.then\(decode/.test(content)) {
        violations.push(relative);
      }
      if (/payload\.getString\(|(?<!ZLink)Message\.from\(|Buffer\.from\(/.test(content)
          && !isAllowedSampleRawBoundaryFile(relative)) {
        violations.push(`${relative}:raw-codec-helper`);
      }
      if (/writeVarint|readVarint|schemaTable|manualSchema|wireType/.test(content)
          && relative !== 'Bingo.Ts/Shared/Contracts/bingo-messages.generated.ts') {
        violations.push(relative);
      }
      if (/addSerializer\s*\(|bingoProtobufSerializer|bingoProtobufContentType/.test(content)) {
        violations.push(relative);
      }
      if (sample === 'Bingo.Ts' && /createProtobufMessage|readProtobufMessage|bingoMessage|readBingoMessage/.test(content)) {
        violations.push(`${relative}:protobuf-message-helper`);
      }
      if (sample === 'Bingo.Ts'
          && /fromBingoProto|toBingoProto/.test(content)
          && !isAllowedBingoRawSessionCodecFile(relative)) {
        violations.push(`${relative}:protobuf-session-helper`);
      }
    }
  }

  assert.deepEqual(missing, []);
  assert.deepEqual(violations, []);
});

test('TicTacToe server uses framework stream session instead of connector framing', () => {
  const checked = [
    'Server/Play/tictactoe-play-module.ts',
    'Server/Play/Infrastructure/ZLink/Actors/play-actor.ts',
    'Server/Play/Infrastructure/ZLink/Sessions/play-session.ts',
    'Server/Play/Infrastructure/ZLink/Sessions/Handlers/authenticate-play-session-handler.ts',
    'Server/Play/Infrastructure/ZLink/Sessions/play-session-factory.ts',
    'Server/Play/Infrastructure/ZLink/Spots/EntrySpot/Handlers/play-actor-join-game-handler.ts',
    'Server/Play/Infrastructure/ZLink/Spots/TicTacToeGameSpot/Handlers/play-actor-place-mark-handler.ts',
    'Shared/Contracts/messages.ts'
  ];
  const missing = [];
  const violations = [];

  for (const relative of checked) {
    const content = fs.readFileSync(path.join(samplesRoot, 'TicTacToe.Ts', relative), 'utf8');
    if (/stream-connector|ZlinkStream(Frame|Codec)|net\.createServer|tryReadFrame/.test(content)) {
      violations.push(relative);
    }
  }

  const playModule = fs.readFileSync(path.join(samplesRoot, 'TicTacToe.Ts', 'Server', 'Play', 'tictactoe-play-module.ts'), 'utf8');
  const playSession = fs.readFileSync(path.join(
    samplesRoot,
    'TicTacToe.Ts',
    'Server',
    'Play',
    'Infrastructure',
    'ZLink',
    'Sessions',
    'play-session.ts'
  ), 'utf8');
  const authenticateHandler = fs.readFileSync(path.join(samplesRoot, 'TicTacToe.Ts', 'Server', 'Play', 'Infrastructure', 'ZLink', 'Sessions', 'Handlers', 'authenticate-play-session-handler.ts'), 'utf8');
  const playActor = fs.readFileSync(path.join(
    samplesRoot,
    'TicTacToe.Ts',
    'Server',
    'Play',
    'Infrastructure',
    'ZLink',
    'Actors',
    'play-actor.ts'
  ), 'utf8');
  const playJoinHandler = fs.readFileSync(path.join(
    samplesRoot,
    'TicTacToe.Ts',
    'Server',
    'Play',
    'Infrastructure',
    'ZLink',
    'Spots',
    'EntrySpot',
    'Handlers',
    'play-actor-join-game-handler.ts'
  ), 'utf8');
  const gameSpot = fs.readFileSync(path.join(
    samplesRoot,
    'TicTacToe.Ts',
    'Server',
    'Play',
    'Infrastructure',
    'ZLink',
    'Spots',
    'TicTacToeGameSpot',
    'tictactoe-game-spot.ts'
  ), 'utf8');
  for (const text of [
    '.addStreamNode(SampleNames.playStream',
    '.registerSession(PlaySessionFactory)',
    'context.client.reply',
    '.getOrCreate(',
    'await actor.push(payload)'
  ]) {
    if (!`${playModule}\n${playSession}\n${authenticateHandler}\n${playActor}\n${playJoinHandler}\n${gameSpot}`.includes(text)) {
      missing.push(text);
    }
  }

  assert.deepEqual(missing, []);
  assert.deepEqual(violations, []);
});

test('node samples keep contracts separate from sample configuration and application roles explicit', () => {
  const ticTacToeContracts = fs.readFileSync(path.join(samplesRoot, 'TicTacToe.Ts', 'Shared', 'Contracts', 'messages.ts'), 'utf8');
  const ticTacToeSettings = fs.readFileSync(path.join(samplesRoot, 'TicTacToe.Ts', 'Server', 'Configuration', 'sample-settings.ts'), 'utf8');
  const ticTacToeCreator = fs.readFileSync(path.join(
    samplesRoot,
    'TicTacToe.Ts',
    'Server',
    'Api',
    'Handlers',
    'create-game-http-handler.ts'
  ), 'utf8');
  const bingoAllocator = fs.readFileSync(path.join(
    samplesRoot,
    'Bingo.Ts',
    'Server',
    'Matchmaking',
    'bingo-match-reservation-store.ts'
  ), 'utf8');
  const required = [
    [ticTacToeSettings, 'SampleNames'],
    [ticTacToeSettings, 'SampleTimings'],
    [ticTacToeCreator, '.create(SampleNames.gameSpotType)'],
    [bingoAllocator, 'class BingoMatchReservationStore']
  ];
  const missing = required
    .filter(([content, text]) => !content.includes(text))
    .map(([, text]) => text);
  const violations = [];
  for (const text of [
    'SampleNames',
    'SampleTimings',
    'TicTacToeGameDirectory',
    'BingoRoomDirectory'
  ]) {
    if (ticTacToeContracts.includes(text)) {
      violations.push(`TicTacToe.Ts/Shared/Contracts/messages.ts:${text}`);
    }
  }

  assert.deepEqual(missing, []);
  assert.deepEqual(violations, []);
});

test('all Node samples use automatic handler registration', () => {
  const apiMain = fs.readFileSync(path.join(samplesRoot, 'TicTacToe.Ts', 'Server', 'Api', 'main.ts'), 'utf8');
  const playMain = fs.readFileSync(path.join(samplesRoot, 'TicTacToe.Ts', 'Server', 'Play', 'main.ts'), 'utf8');
  const apiModule = fs.readFileSync(path.join(samplesRoot, 'TicTacToe.Ts', 'Server', 'Api', 'tictactoe-api-module.ts'), 'utf8');
  const playModule = fs.readFileSync(path.join(samplesRoot, 'TicTacToe.Ts', 'Server', 'Play', 'tictactoe-play-module.ts'), 'utf8');
  const apiHandler = fs.readFileSync(path.join(
    samplesRoot,
    'TicTacToe.Ts',
    'Server',
    'Api',
    'Handlers',
    'authenticate-player-handler.ts'
  ), 'utf8');
  const playHandler = fs.readFileSync(path.join(
    samplesRoot,
    'TicTacToe.Ts',
    'Server',
    'Play',
    'Infrastructure',
    'ZLink',
    'Spots',
    'EntrySpot',
    'Handlers',
    'play-actor-join-game-handler.ts'
  ), 'utf8');
  const ticTacToeTimerHandler = fs.readFileSync(path.join(
    samplesRoot,
    'TicTacToe.Ts',
    'Server',
    'Play',
    'Infrastructure',
    'ZLink',
    'Spots',
    'TicTacToeGameSpot',
    'Handlers',
    'tictactoe-game-timer-handler.ts'
  ), 'utf8');
  const bingoPlayModule = fs.readFileSync(path.join(samplesRoot, 'Bingo.Ts', 'Server', 'Play', 'bingo-play-module.ts'), 'utf8');
  const bingoTimerHandler = fs.readFileSync(path.join(
    samplesRoot,
    'Bingo.Ts',
    'Server',
    'Play',
    'Infrastructure',
    'ZLink',
    'Spots',
    'BingoRoomSpot',
    'Handlers',
    'bingo-room-timer-handler.ts'
  ), 'utf8');
  const nestPackage = sampleSourceFiles(path.join(workspaceRoot, 'packages', 'nestjs', 'src'))
    .map((file) => fs.readFileSync(file, 'utf8'))
    .join('\n');
  const required = [
    [nestPackage, 'export function zlinkRequestHandler'],
    [nestPackage, 'export function zlinkSpotTimerHandler'],
    [apiModule, 'zlinkModule(__dirname'],
    [apiModule, ".addHandlerGroup('api')"],
    [apiHandler, "@zlinkRequestHandler('api', PacketNames.authenticatePlayerReq)"],
    [playModule, 'objectServer.addSpotFactory('],
    [playModule, 'zlinkModule(__dirname'],
    [playHandler, '@zlinkEntrySpotActorRequestHandler({'],
    [ticTacToeTimerHandler, 'class TicTacToeGameTimerHandler'],
    [ticTacToeTimerHandler, '@zlinkSpotTimerHandler({'],
    [bingoTimerHandler, 'class BingoRoomTimerHandler'],
    [bingoTimerHandler, '@zlinkSpotTimerHandler({'],
    [fs.readFileSync(path.join(samplesRoot, 'Bingo.Ts', 'Server', 'Api', 'bingo-api-module.ts'), 'utf8'), '.addHandlerGroup(\'api\')'],
    [fs.readFileSync(path.join(samplesRoot, 'Bingo.Ts', 'Server', 'Matchmaking', 'bingo-matchmaking-module.ts'), 'utf8'),
      '.addInstanceSpotFactory('],
    [fs.readFileSync(path.join(samplesRoot, 'DeliveryDispatch.Ts', 'Server', 'Session', 'customer-status-handler.ts'), 'utf8'),
      '@zlinkEntrySpotActorSendHandler({'],
    [fs.readFileSync(path.join(samplesRoot, 'DeliveryDispatch.Ts', 'Server', 'Session', 'customer-status-handler.ts'), 'utf8'),
      'packetName: PacketNames.deliveryStatusUpdated'],
    [bingoPlayModule, 'zlinkModule(__dirname'],
    [playModule, '.addStreamNode(SampleNames.playStream']
  ];
  const missing = required
    .filter(([content, text]) => !content.includes(text))
    .map(([, text]) => text);
  const violations = [];
  if (nestPackage.includes('export function zlinkHandlers')) {
    violations.push('@zlink-systems/nestjs:zlinkHandlers');
  }
  for (const [name, content] of [
    ['TicTacToe.Ts/Server/Api/Handlers/authenticate-player-handler.ts', apiHandler],
    ['TicTacToe.Ts/Server/Play/Infrastructure/ZLink/Spots/EntrySpot/Handlers/play-actor-join-game-handler.ts', playHandler],
    ['TicTacToe.Ts/Server/Play/Infrastructure/ZLink/Spots/TicTacToeGameSpot/Handlers/tictactoe-game-timer-handler.ts', ticTacToeTimerHandler],
    ['Bingo.Ts/Server/Play/Infrastructure/ZLink/Spots/BingoRoomSpot/Handlers/bingo-room-timer-handler.ts', bingoTimerHandler]
  ]) {
    if (/zlink(?:Request|Send|Publish|SpotActorRequest|EntrySpotActorRequest|SpotTimer)Handler\([^;\n]*\)\([A-Z]/.test(content)) {
      violations.push(`${name}:manual-decorator-call`);
    }
  }
  for (const [name, content] of [
    ['TicTacToe.Ts/Server/Api/main.ts', apiMain],
    ['TicTacToe.Ts/Server/Play/main.ts', playMain],
    ['Bingo.Ts/Server/Api/main.ts', fs.readFileSync(path.join(samplesRoot, 'Bingo.Ts', 'Server', 'Api', 'main.ts'), 'utf8')],
    ['Bingo.Ts/Server/Play/main.ts', fs.readFileSync(path.join(samplesRoot, 'Bingo.Ts', 'Server', 'Play', 'main.ts'), 'utf8')],
    ['Bingo.Ts/Server/Session/main.ts', fs.readFileSync(path.join(samplesRoot, 'Bingo.Ts', 'Server', 'Session', 'main.ts'), 'utf8')]
  ]) {
    if (content.includes('zlinkHandlers')) {
      violations.push(name);
    }
  }
  for (const text of [
    'providers: [',
    "require('@nestjs/common')",
    'ZLinkModule.forRoot',
    'AuthenticatePlayerHandler',
    'PlayActorJoinGameHandler',
    'PlayActorPlaceMarkHandler'
  ]) {
    for (const [name, content] of [
      ['TicTacToe.Ts/Server/Api/main.ts', apiMain],
      ['TicTacToe.Ts/Server/Play/main.ts', playMain]
    ]) {
      if (content.includes(text)) {
        violations.push(`${name}:${text}`);
      }
    }
  }
  for (const sample of requiredSamples) {
    for (const file of sampleSourceFiles(path.join(samplesRoot, sample))) {
      const content = fs.readFileSync(file, 'utf8');
      if (/\.add(?:Request|Send|Publish)Handler\(/.test(content)
        || /\.addSubscribe\(/.test(content)
        || /\.handlers\.add(?:Handler|Packet|Subscribe|ActorPacket)\(/.test(content)
        || /\.actor(?:Request|Send)\(/.test(content)
        || /\.packet\(/.test(content)) {
        violations.push(`${relativePath(samplesRoot, file)}:manual-handler-registration`);
      }
      if (/@ZLink(?:Request|Send|Publish|SpotRequest|SpotSubscription|SpotActorSend|SpotActorRequest|StreamPacket|StreamRaw)\(/.test(content)) {
        violations.push(`${relativePath(samplesRoot, file)}:handler-missing-nest-discovery-metadata`);
      }
      if (
        file.endsWith('-module.ts')
        && content.includes('ZLinkModule.forRoot')
        && !content.includes('zlinkModule(__dirname')
      ) {
        violations.push(`${relativePath(samplesRoot, file)}:automatic-discovery-disabled`);
      }
    }
  }

  assert.deepEqual(missing, []);
  assert.deepEqual(violations, []);
});

test('session samples that bind actors enable Framework actor dispatch', () => {
  const modules = [
    ['Bingo.Ts', 'Server', 'Session', 'bingo-session-module.ts'],
    ['TicTacToe.Ts', 'Server', 'Play', 'tictactoe-play-module.ts'],
    ['SupportChat.Ts', 'Server', 'Session', 'supportchat-session-module.ts'],
    ['DeliveryDispatch.Ts', 'Server', 'Session', 'session-module.ts'],
    ['DeliveryDispatch.Ts', 'Server', 'CourierSession', 'courier-session-module.ts'],
    ['GameQuest.Ts', 'Server', 'GameApi', 'game-api-module.ts'],
    ['ZoneWorld', 'Server', 'Gateway', 'gateway-module.ts']
  ];
  const missing = modules
    .filter((segments) => {
      const source = fs.readFileSync(path.join(samplesRoot, ...segments), 'utf8');
      return !source.includes('.enableActorDispatch()');
    })
    .map((segments) => segments.join('/'));

  assert.deepEqual(missing, []);
});

test('TicTacToe keeps manual topology on one physical MeshNode per process', () => {
  const apiModule = fs.readFileSync(path.join(
    samplesRoot,
    'TicTacToe.Ts',
    'Server',
    'Api',
    'tictactoe-api-module.ts'
  ), 'utf8');
  const playModule = fs.readFileSync(path.join(
    samplesRoot,
    'TicTacToe.Ts',
    'Server',
    'Play',
    'tictactoe-play-module.ts'
  ), 'utf8');
  const createGame = fs.readFileSync(path.join(
    samplesRoot,
    'TicTacToe.Ts',
    'Server',
    'Api',
    'Handlers',
    'create-game-http-handler.ts'
  ), 'utf8');
  const authenticate = fs.readFileSync(path.join(
    samplesRoot,
    'TicTacToe.Ts',
    'Server',
    'Play',
    'Infrastructure',
    'ZLink',
    'Sessions',
    'Handlers',
    'authenticate-play-session-handler.ts'
  ), 'utf8');

  for (const module of [apiModule, playModule]) {
    assert.equal((module.match(/\.addRouteMesh\(/g) ?? []).length, 1);
    assert.match(module, /addRouteMesh\(SampleNames\.playSpotNode\)/);
    assert.match(module, /channel\(SampleNames\.apiChannel\)/);
    assert.match(module, /peerConnections\(\)\.connect\(/);
  }
  assert.match(apiModule, /\.addHandlerGroup\('api'\)/);
  assert.match(apiModule, /mesh\.objects\(\)\.client\(\)/);
  assert.match(playModule, /objectServer\.addSpotFactory\(/);
  assert.match(createGame, /\.create\(SampleNames\.gameSpotType\)/);
  assert.doesNotMatch(createGame, /requestToChannel|ownerPlayEndpoint|NodeRid/);
  assert.match(authenticate, /requestToChannel\(\s*SampleNames\.apiChannel/);
});

test('only TicTacToe uses manual server-to-server connections', () => {
  const ticTacToeServer = sampleSourceFiles(path.join(samplesRoot, 'TicTacToe.Ts', 'Server'))
    .map((file) => fs.readFileSync(file, 'utf8'))
    .join('\n');
  assert.match(ticTacToeServer, /\.peerConnections\(\)\.connect\(/);

  const violations = [];
  for (const sample of requiredSamples.filter((name) => name !== 'TicTacToe.Ts')) {
    for (const file of sampleSourceFiles(path.join(samplesRoot, sample, 'Server'))) {
      const content = fs.readFileSync(file, 'utf8');
      if (/\.peerConnections\(\)\.connect\(/.test(content)
        || /\.enableClient\(\s*[^)]/.test(content)
        || /\.enableSubscriber\(\s*[^)]/.test(content)
        || /\.enablePubSub\([^,\n]+,[^,\n]+,[^)]+\)/.test(content)
        || /ZLinkHttpClient\.create\(/.test(content)) {
        violations.push(relativePath(samplesRoot, file));
      }
    }
  }
  assert.deepEqual(violations, []);
});

test('Bingo TypeScript sample separates room lifecycle from pure bingo game rules', () => {
  const roomGame = fs.readFileSync(path.join(
    samplesRoot,
    'Bingo.Ts',
    'Server',
    'Play',
    'Domain',
    'Bingo',
    'bingo-room-game.ts'
  ), 'utf8');
  const bingoGame = fs.readFileSync(path.join(
    samplesRoot,
    'Bingo.Ts',
    'Server',
    'Play',
    'Domain',
    'Bingo',
    'bingo-game.ts'
  ), 'utf8');
  const required = [
    [bingoGame, 'class BingoGame'],
    [bingoGame, 'submitCard'],
    [bingoGame, 'drawNext'],
    [bingoGame, 'this.winners.push'],
    [roomGame, 'new BingoGame'],
    [roomGame, 'this.game.submitCard'],
    [roomGame, 'this.game.drawNext']
  ];
  const missing = required
    .filter(([content, text]) => !content.includes(text))
    .map(([, text]) => text);
  const violations = [];
  for (const text of [
    'new BingoCard',
    'this.winners.push',
    'player.card.mark('
  ]) {
    if (roomGame.includes(text)) {
      violations.push(text);
    }
  }

  assert.deepEqual(missing, []);
  assert.deepEqual(violations, []);
});

test('Bingo TypeScript sample normalizes wire room settings before creating room state', () => {
  const roomSpot = fs.readFileSync(path.join(
    samplesRoot,
    'Bingo.Ts',
    'Server',
    'Play',
    'Infrastructure',
    'ZLink',
    'Spots',
    'BingoRoomSpot',
    'bingo-room-spot.ts'
  ), 'utf8');
  const roomModels = fs.readFileSync(path.join(
    samplesRoot,
    'Bingo.Ts',
    'Server',
    'Play',
    'Domain',
    'Bingo',
    'bingo-room-models.ts'
  ), 'utf8');

  assert.match(roomModels, /function roomSettingsFromPayload/);
  assert.match(roomSpot, /const settings = request\.decode<BingoRoomSettingsInput>/);
  assert.match(roomSpot, /roomSettingsFromPayload\(settings\)/);
  assert.doesNotMatch(roomSpot, /protobufSerializer\.deserialize/);
});

test('Bingo TypeScript sample exposes spot actor contracts explicitly', () => {
  const playModule = fs.readFileSync(path.join(samplesRoot, 'Bingo.Ts', 'Server', 'Play', 'bingo-play-module.ts'), 'utf8');
  const roomSpot = fs.readFileSync(path.join(
    samplesRoot,
    'Bingo.Ts',
    'Server',
    'Play',
    'Infrastructure',
    'ZLink',
    'Spots',
    'BingoRoomSpot',
    'bingo-room-spot.ts'
  ), 'utf8');
  const entrySpot = fs.readFileSync(path.join(
    samplesRoot,
    'Bingo.Ts',
    'Server',
    'Play',
    'Infrastructure',
    'ZLink',
    'Spots',
    'EntrySpot',
    'bingo-entry-spot.ts'
  ), 'utf8');
  const matchHandler = fs.readFileSync(path.join(
    samplesRoot,
    'Bingo.Ts',
    'Server',
    'Play',
    'Infrastructure',
    'ZLink',
    'Spots',
    'EntrySpot',
    'Handlers',
    'match-bingo-actor-handler.ts'
  ), 'utf8');
  const submitHandler = fs.readFileSync(path.join(
    samplesRoot,
    'Bingo.Ts',
    'Server',
    'Play',
    'Infrastructure',
    'ZLink',
    'Spots',
    'BingoRoomSpot',
    'Handlers',
    'submit-bingo-card-handler.ts'
  ), 'utf8');
  const frameworkSpotContract = fs.readFileSync(path.join(
    workspaceRoot,
    'packages',
    'framework',
    'src',
    'contracts',
    'Spots',
    'ZLinkSpot.ts'
  ), 'utf8');
  const required = [
    [frameworkSpotContract, 'interface ZLinkSpot<TActor extends ZLinkActor = ZLinkActor>'],
    [frameworkSpotContract, 'interface ZLinkEntrySpot<TActor extends ZLinkActor = ZLinkActor>'],
    [playModule, '.addActorFactory(\n            SampleNames.playerActorType,\n            PlayerActorFactory,'],
    [playModule, '.addRouteMesh(SampleNames.roomSpotNode'],
    [playModule, '.addEntrySpot(BingoEntrySpot)'],
    [playModule, '.addSpotFactory(\n            SampleNames.roomSpotType,\n            BingoRoomSpot,'],
    [roomSpot, 'implements ZLinkSpot<PlayerActor>'],
    // node spec `04-spots.ko.md:65` declares onActorJoin(actorId: string, request).
    [roomSpot, 'onActorJoin(actorId: string'],
    [roomSpot, 'onJoinedActor(actor: PlayerActor'],
    [roomSpot, 'onLeaveActor(actor: PlayerActor'],
    [roomSpot, 'onDisconnectActor(_actor: PlayerActor'],
    [entrySpot, 'implements ZLinkEntrySpot<PlayerActor>'],
    [entrySpot, 'onJoinedActor(actor: PlayerActor'],
    [entrySpot, 'onLeaveActor(actor: PlayerActor'],
    [entrySpot, 'onDisconnectActor(_actor: PlayerActor'],
    [matchHandler, 'zlinkEntrySpotActorRequestHandler'],
    [matchHandler, 'entrySpot: () => BingoEntrySpot'],
    [matchHandler, 'actor: () => PlayerActor'],
    [matchHandler, 'packetName: PacketNames.matchBingoReq'],
    [matchHandler, 'implements ZLinkEntrySpotActorRequestHandler<BingoEntrySpot, PlayerActorType, MatchBingoReq, MatchBingoRes>'],
    [matchHandler, 'hostActorId: actor.actorId'],
    [submitHandler, 'zlinkSpotActorRequestHandler'],
    [submitHandler, 'spot: () => BingoRoomSpot'],
    [submitHandler, 'actor: () => PlayerActor'],
    [submitHandler, 'packetName: PacketNames.submitBingoCardReq'],
    [submitHandler, 'implements ZLinkSpotActorRequestHandler<BingoRoomSpot, PlayerActor, SubmitBingoCardReq, SubmitBingoCardRes>']
  ];
  const missing = required
    .filter(([content, text]) => !content.includes(text))
    .map(([, text]) => text);

  const violations = [];
  for (const [name, content] of [
    ['Bingo.Ts/Server/Play/Infrastructure/ZLink/Spots/BingoRoomSpot/bingo-room-spot.ts', roomSpot],
    ['Bingo.Ts/Server/Play/Infrastructure/ZLink/Spots/EntrySpot/bingo-entry-spot.ts', entrySpot]
  ]) {
    if (content.includes('addActorPacket')) {
      violations.push(name);
    }
    for (const forbidden of ['Map<string, PlayerActor>', 'actor.attachRoom(']) {
      if (content.includes(forbidden)) violations.push(`${name}:${forbidden}`);
    }
  }

  assert.deepEqual(missing, []);
  assert.deepEqual(violations, []);
});

test('node TypeScript samples schedule actor destroy in Entry Spot without mutable lifecycle actors', () => {
  const cases = [
    {
      sample: 'Bingo.Ts',
      actor: ['Server', 'Play', 'Infrastructure', 'ZLink', 'Actors', 'player-actor.ts'],
      entrySpot: ['Server', 'Play', 'Infrastructure', 'ZLink', 'Spots', 'EntrySpot', 'bingo-entry-spot.ts'],
      userSpot: ['Server', 'Play', 'Infrastructure', 'ZLink', 'Spots', 'BingoRoomSpot', 'bingo-room-spot.ts']
    },
    {
      sample: 'TicTacToe.Ts',
      actor: ['Server', 'Play', 'Infrastructure', 'ZLink', 'Actors', 'play-actor.ts'],
      entrySpot: ['Server', 'Play', 'Infrastructure', 'ZLink', 'Spots', 'EntrySpot', 'play-entry-spot.ts'],
      userSpot: ['Server', 'Play', 'Infrastructure', 'ZLink', 'Spots', 'TicTacToeGameSpot', 'tictactoe-game-spot.ts']
    }
  ];
  const missing = [];

  for (const sample of cases) {
    const actor = fs.readFileSync(path.join(samplesRoot, sample.sample, ...sample.actor), 'utf8');
    const entrySpot = fs.readFileSync(path.join(samplesRoot, sample.sample, ...sample.entrySpot), 'utf8');
    const userSpot = fs.readFileSync(path.join(samplesRoot, sample.sample, ...sample.userSpot), 'utf8');
    const runSample = fs.readFileSync(path.join(samplesRoot, 'run-sample.mjs'), 'utf8');

    for (const [label, content, text] of [
      ['entrySpot', entrySpot, 'onJoinedActor'],
      ['entrySpot', entrySpot, 'destroyActor'],
      ['entrySpot', entrySpot, 'onDisconnectActor'],
      ['userSpot', userSpot, 'onDisconnectActor'],
      ['runner', runSample, 'scripts/browser-e2e/run-sample.mjs']
    ]) {
      if (!content.includes(text)) {
        missing.push(`${sample.sample}:${label}:${text}`);
      }
    }
    if (userSpot.includes('destroyActor')) {
      missing.push(`${sample.sample}:userSpot:destroyActor`);
    }
    for (const forbidden of ['destroyAfterEntrySpotJoin', 'markForDestroyAfterRoomLeave', 'markDisconnected']) {
      if (actor.includes(forbidden)) missing.push(`${sample.sample}:actor:${forbidden}`);
    }
    assert.match(userSpot, /onJoinedActor\(actor: [A-Za-z]+Actor/);
    assert.match(userSpot, /onLeaveActor\(actor: [A-Za-z]+Actor/);
  }

  assert.deepEqual(missing, []);
});

test('node sample wrappers delegate shared mechanics and sample-specific orchestration', () => {
  const sharedRunner = fs.readFileSync(path.join(samplesRoot, 'run-sample.mjs'), 'utf8');
  assert.match(sharedRunner, /await import\(pathToFileURL/);
  assert.match(sharedRunner, /await startRedis\(\)/);
  assert.match(sharedRunner, /scripts\/browser-e2e\/run-sample\.mjs/);
  assert.doesNotMatch(sharedRunner, /sampleDefinitions|Bingo\.Ts|TicTacToe\.Ts|DeliveryDispatch\.Ts/);

  for (const sample of topologySamples) {
    const shell = fs.readFileSync(path.join(samplesRoot, sample, 'run_sample.sh'), 'utf8');
    const powershell = fs.readFileSync(path.join(samplesRoot, sample, 'run_sample.ps1'), 'utf8');
    const sampleRunner = fs.readFileSync(path.join(samplesRoot, sample, 'Runner', 'sample-runner.mjs'), 'utf8');
    const client = fs.readFileSync(path.join(samplesRoot, sample, 'Client', 'main.ts'), 'utf8');

    assert.match(shell, /run-sample\.mjs" .*Runner\/sample-runner\.mjs/);
    assert.match(powershell, /run-sample\.mjs.*Runner\/sample-runner\.mjs/);
    assert.doesNotMatch(shell, new RegExp(`run-sample\\.mjs" ${escapeRegExp(sample)}(?:\\s|$)`));
    assert.doesNotMatch(powershell, new RegExp(`run-sample\\.mjs.*["']${escapeRegExp(sample)}["']`));
    assert.doesNotMatch(shell, /docker|grep|start_role|start_server|wait_port/);
    assert.doesNotMatch(powershell, /docker|Select-String|Start-Role|Start-Server|Wait-Port/i);
    assert.match(sampleRunner, /export async function runSample\(ctx\)/);
    assert.match(client, /loadSampleConfig/);
    assert.doesNotMatch(client, /child_process|spawn\(|fork\(|execFile/);
  }
});

test('node shared sample runner isolates Redis and application ports without Docker volumes', () => {
  const runner = fs.readFileSync(path.join(samplesRoot, 'run-sample.mjs'), 'utf8');

  assert.match(runner, /'create', '--name', name, '--tmpfs', '\/data', '-p', '127\.0\.0\.1::6379'/);
  assert.match(runner, /docker'\), \['rm', '-fv', redisContainer\]/);
  assert.match(runner, /reserveBrowserSafePort/);
  assert.match(runner, /30000 \+ Math\.floor\(Math\.random\(\) \* 10000\)/);
  assert.match(runner, /printLogs\(\)/);
  assert.doesNotMatch(runner, /127\.0\.0\.1:\d{4,5}/);
});

test('framework aggregate runners never remove Redis containers or processes owned by another run', () => {
  const shellRunner = fs.readFileSync(path.join(samplesRoot, 'run_samples.sh'), 'utf8');
  const powershellRunner = fs.readFileSync(path.join(samplesRoot, 'run_samples.ps1'), 'utf8');
  const e2eRunner = fs.readFileSync(path.join(workspaceRoot, 'e2e/run_e2e_all.sh'), 'utf8');
  const redisHelper = fs.readFileSync(path.join(workspaceRoot, 'e2e/redis-container.sh'), 'utf8');
  const frameworkRoot = path.resolve(workspaceRoot, '..', '..');

  for (const [label, content] of [
    ['samples:sh', shellRunner],
    ['samples:ps1', powershellRunner],
    ['e2e:sh', e2eRunner],
    ['redis-helper:sh', redisHelper],
    ['dotnet:samples', fs.readFileSync(path.join(frameworkRoot, 'languages/dotnet/samples/run_samples.sh'), 'utf8')],
    ['dotnet:e2e', fs.readFileSync(path.join(frameworkRoot, 'languages/dotnet/e2e/run_e2e_all.sh'), 'utf8')],
    ['java:samples', fs.readFileSync(path.join(frameworkRoot, 'languages/java/samples/run_samples.sh'), 'utf8')],
    ['java:e2e', fs.readFileSync(path.join(frameworkRoot, 'languages/java/e2e/run_e2e_all.sh'), 'utf8')],
    ['kotlin:e2e', fs.readFileSync(path.join(frameworkRoot, 'languages/java/e2e-kotlin/run_e2e_all.sh'), 'utf8')],
    ['cpp:samples', fs.readFileSync(path.join(frameworkRoot, 'languages/cpp/samples/run_samples.sh'), 'utf8')],
    ['cpp:e2e', fs.readFileSync(path.join(frameworkRoot, 'languages/cpp/e2e/run_e2e_all.sh'), 'utf8')]
  ]) {
    assert.doesNotMatch(content, /zlink_redis_cleanup_scope|docker ps -a|pkill\s/,
      `${label} must only clean resources created by its own sample or E2E run`);
  }
});

test('node samples keep only contracts and shared sample configuration under Shared', () => {
  const violations = [];
  for (const sample of requiredSamples) {
    const sharedRoot = path.join(samplesRoot, sample, 'Shared');
    for (const file of sampleSourceFiles(sharedRoot)) {
      const relative = relativePath(path.join(samplesRoot, sample), file);
      if (!relative.startsWith('Shared/Contracts/')
        && relative !== 'Shared/Configuration/sample-names.ts') {
        violations.push(relative);
      }
    }
  }
  assert.deepEqual(violations, []);
});

test('node top-level sample runners execute every maintained sample', () => {
  const shellRunner = fs.readFileSync(path.join(samplesRoot, 'run_samples.sh'), 'utf8');
  const powershellRunner = fs.readFileSync(path.join(samplesRoot, 'run_samples.ps1'), 'utf8');
  const missing = [];

  for (const sample of maintainedSamples) {
    if (!shellRunner.includes(sample)) {
      missing.push(`sh:${sample}`);
    }
    if (!powershellRunner.includes(sample)) {
      missing.push(`ps1:${sample}`);
    }
  }

  assert.deepEqual(missing, []);
});

test('node session samples do not implement sample-only actor session stores', () => {
  const bannedPatterns = [
    /SessionBindingTable/,
    /BoundNotificationHub/,
    /bindings\s*=\s*new Map\(/,
    /notificationHub/,
    /sessionFor\(actorId\)/,
    /staleSend\(actorId/
  ];
  const violations = [];

  for (const sample of ['TicTacToe.Ts', 'Bingo.Ts']) {
    for (const file of sampleSourceFiles(path.join(samplesRoot, sample))) {
      const content = fs.readFileSync(file, 'utf8');
      for (const pattern of bannedPatterns) {
        if (pattern.test(content)) {
          violations.push(`${relativePath(samplesRoot, file)} matches ${pattern}`);
        }
      }
    }
  }

  assert.deepEqual(violations, []);
});

test('node samples do not keep unreachable TypeScript files', () => {
  const violations = findUnreachableSampleTypeScriptFiles();

  assert.deepEqual(violations, []);
});

test('node framework source tree does not keep emitted JavaScript beside TypeScript sources', () => {
  const srcRoot = path.join(workspaceRoot, 'packages', 'framework', 'src');
  const emitted = listFiles(srcRoot)
    .filter((file) => file.endsWith('.js'))
    .map((file) => relativePath(workspaceRoot, file))
    .sort();

  assert.deepEqual(emitted, []);
});

test('node run_samples.sh executes every sample self-check', () => {
  if (process.platform !== 'linux') {
    return;
  }

  const output = childProcess.execFileSync(path.join(samplesRoot, 'run_samples.sh'), {
    cwd: workspaceRoot,
    encoding: 'utf8'
  });

  for (const sample of maintainedSamples) {
    assert.match(output, new RegExp(`PASS ${escapeRegExp(sample)}`));
  }
});

test('node cross-language smoke covers bidirectional channel fanout route stream drain and store paths', () => {
  const smoke = fs.readFileSync(path.join(workspaceRoot, 'cross-language', 'node_dotnet_smoke.js'), 'utf8');
  const required = [
    'requestToChannel',
    'sendToChannel',
    ".publish('profiles'",
    'nodePublisherToDotnetFanoutSubscriber',
    'dotnetPublisherToNodeFanoutSubscriber',
    'nodeRouteClientToDotnetRouteServer',
    'dotnetRouteClientToNodeRouteServer',
    'nodeConnectorToDotnetStreamServer',
    'dotnetConnectorToNodeStreamServer',
    'nodeConnectorObservesDotnetSessionClosing',
    'dotnetConnectorObservesNodeSessionClosing',
    'nodeDotnetRedisLocationRows'
  ];
  const missing = required.filter((text) => !smoke.includes(text));

  assert.deepEqual(missing, []);
});

function sampleSourceFiles(root) {
  const files = [];
  for (const entry of fs.readdirSync(root, { withFileTypes: true })) {
    const fullPath = path.join(root, entry.name);
    if (entry.isDirectory()) {
      if (entry.name === 'dist' || entry.name === 'node_modules') {
        continue;
      }
      files.push(...sampleSourceFiles(fullPath));
    } else if (entry.isFile() && /\.(?:js|ts|mjs|cjs|md|sh|ps1|proto)$/.test(entry.name)) {
      files.push(fullPath);
    }
  }
  return files;
}

function listFiles(root) {
  const files = [];
  for (const entry of fs.readdirSync(root, { withFileTypes: true })) {
    const fullPath = path.join(root, entry.name);
    if (entry.isDirectory()) {
      if (entry.name === 'dist' || entry.name === 'node_modules') {
        continue;
      }
      files.push(...listFiles(fullPath));
    } else if (entry.isFile()) {
      files.push(fullPath);
    }
  }
  return files;
}

function findUnreachableSampleTypeScriptFiles() {
  const files = new Set(listFiles(samplesRoot).filter((file) => file.endsWith('.ts')));
  const used = new Set();
  const queue = [];

  function add(file) {
    const normalized = path.normalize(file);
    if (files.has(normalized) && !used.has(normalized)) {
      used.add(normalized);
      queue.push(normalized);
    }
  }

  for (const sample of maintainedSamples) {
    // Discover each role entry point dynamically. A role may use a qualified name
    // when two instances share one directory, such as `node1-main.ts`.
    for (const file of listFiles(path.join(samplesRoot, sample))) {
      if (/(?:^|-)main\.ts$/.test(path.basename(file))) {
        add(file);
      }
    }
    const runner = path.join(samplesRoot, sample, 'Runner', 'sample-runner.mjs');
    if (fs.existsSync(runner)) {
      const content = fs.readFileSync(runner, 'utf8');
      for (const match of content.matchAll(/dist\/([^'"`]+)\.js/g)) {
        add(path.join(samplesRoot, sample, `${match[1]}.ts`));
      }
    }
  }
  while (queue.length > 0) {
    const file = queue.shift();
    const content = fs.readFileSync(file, 'utf8');
    addDiscoveredProviderFiles(file, content, add, files);
    for (const specifier of importSpecifiers(content)) {
      const resolved = resolveSampleImport(file, specifier, files);
      if (resolved !== null) {
        add(resolved);
      }
    }
  }

  return [...files]
    .filter((file) => !used.has(file))
    .map((file) => relativePath(samplesRoot, file))
    .sort();
}

function addDiscoveredProviderFiles(file, content, add, files) {
  const discoveryPatterns = [/zlinkDiscoverProviders\(path\.join\(__dirname,\s*([^)]*)\)\)/g];
  if (content.includes('providerDiscovery')) {
    discoveryPatterns.push(/path\.join\(__dirname,\s*([^)]*)\)/g);
  }
  if (content.includes('zlinkModule(__dirname')) {
    for (const discoveredRoot of defaultZLinkProviderDiscoveryRoots(path.dirname(file))) {
      for (const candidate of files) {
        if (candidate.startsWith(`${discoveredRoot}${path.sep}`)) {
          add(candidate);
        }
      }
    }
  }

  for (const discoveryPattern of discoveryPatterns) {
    for (const match of content.matchAll(discoveryPattern)) {
      const parts = [...match[1].matchAll(/'([^']+)'/g)].map((part) => part[1]);
      if (parts.length === 0) {
        continue;
      }
      const discoveredRoot = path.resolve(path.dirname(file), ...parts);
      for (const candidate of files) {
        if (candidate.startsWith(`${discoveredRoot}${path.sep}`)) {
          add(candidate);
        }
      }
    }
  }
}

function defaultZLinkProviderDiscoveryRoots(roleRoot) {
  return fs.existsSync(roleRoot) ? [roleRoot] : [];
}

function importSpecifiers(content) {
  const specifiers = [];
  for (const pattern of [
    /require\(['"]([^'"]+)['"]\)/g,
    /from ['"]([^'"]+)['"]/g,
    /import ['"]([^'"]+)['"]/g,
    /import\(['"]([^'"]+)['"]\)/g
  ]) {
    for (const match of content.matchAll(pattern)) {
      specifiers.push(match[1]);
    }
  }
  return specifiers;
}

function resolveSampleImport(fromFile, specifier, files) {
  if (!specifier.startsWith('.')) {
    return null;
  }
  const base = path.resolve(path.dirname(fromFile), specifier);
  for (const candidate of [
    `${base}.ts`,
    path.join(base, 'index.ts')
  ]) {
    if (files.has(candidate)) {
      return candidate;
    }
  }
  return null;
}

function assertOrdered(name, content, snippets) {
  let offset = 0;
  for (const snippet of snippets) {
    const index = content.indexOf(snippet, offset);
    assert.notEqual(index, -1, `${name} is missing ordered scenario snippet: ${snippet}`);
    offset = index + snippet.length;
  }
}

function readSample(sample, relative) {
  return fs.readFileSync(path.join(samplesRoot, sample, relative), 'utf8');
}

function relativePath(base, file) {
  return path.relative(base, file).split(path.sep).join('/');
}

function isAllowedBingoRawSessionCodecFile(relative) {
  return [
    'Bingo.Ts/Shared/Contracts/messages.ts',
    'Bingo.Ts/Shared/Contracts/protobuf-codec.ts',
    'Bingo.Ts/Server/Session/main.ts'
  ].includes(relative);
}

function isAllowedSampleRawBoundaryFile(relative) {
  return [
    'Bingo.Ts/Shared/Contracts/protobuf-codec.ts',
    'Bingo.Ts/Server/Api/bingo-api-module.ts',
    'Bingo.Ts/Server/Play/bingo-play-module.ts',
    'Bingo.Ts/Server/Session/bingo-session-module.ts',
    'Bingo.Ts/Server/Play/Infrastructure/ZLink/Matchmaking/redis-bingo-match-queue.ts',
    'TicTacToe.Ts/Server/Configuration/redis-room-route-store.ts'
  ].includes(relative);
}

function isAllowedBingoCodecConfigurationFile(relative) {
  return [
    'Bingo.Ts/Server/Api/bingo-api-module.ts',
    'Bingo.Ts/Server/Play/bingo-play-module.ts',
    'Bingo.Ts/Server/Session/bingo-session-module.ts'
  ].includes(relative);
}

function escapeRegExp(value) {
  return value.replace(/[.*+?^${}()|[\]\\]/g, '\\$&');
}
