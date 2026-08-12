const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const test = require('node:test');

const root = path.resolve(__dirname, '../..');

function read(relative) {
  return fs.readFileSync(path.join(root, relative), 'utf8');
}

test('sample wire names describe the framework call method', () => {
  const gameQuestMessages = read('samples/GameQuest.Ts/Shared/Contracts/messages.ts');
  const gameQuestClient = read('samples/GameQuest.Ts/Client/gamequest-client-scenario.ts');
  const gameQuestHandlers = read(
    'samples/GameQuest.Ts/Server/GameApi/Infrastructure/ZLink/gamequest-player-handlers.ts'
  );
  const gameQuestNotifications = read(
    'samples/GameQuest.Ts/Server/QuestMission/Infrastructure/ZLink/player-quest-notifier.ts'
  );

  assert.match(gameQuestMessages, /class CollectItemMsg\b/);
  assert.match(gameQuestMessages, /class EnterAreaMsg\b/);
  assert.doesNotMatch(gameQuestMessages, /CollectItem(?:Req|Res)|EnterArea(?:Req|Res)/);
  assert.match(gameQuestClient, /\.send\(collectItemMsg\(/);
  assert.match(gameQuestClient, /\.send\(enterAreaMsg\(/);
  assert.match(gameQuestHandlers, /zlinkEntrySpotActorSendHandler[\s\S]*?PacketNames\.collectItemMsg/);
  assert.match(gameQuestHandlers, /zlinkEntrySpotActorSendHandler[\s\S]*?PacketNames\.enterAreaMsg/);
  assert.match(gameQuestNotifications, /sendToActor\([\s\S]*?new DeliverQuestNotificationMsg\(/);

  const zoneContracts = read('samples/ZoneWorld/Shared/contracts.ts');
  const zoneClient = read('samples/ZoneWorld/Client/main.ts');
  const zoneHandlers = read(
    'samples/ZoneWorld/Server/ZoneNode/Infrastructure/ZLink/Handlers/player-handlers.ts'
  );
  assert.match(zoneContracts, /class MessageFollowProbeReq\b/);
  assert.match(zoneContracts, /class MessageFollowProbeMsg\b/);
  assert.match(zoneContracts, /class EnterZoneReq\b/);
  assert.doesNotMatch(zoneContracts, /class EnterZoneMsg\b/);
  assert.match(zoneClient, /\.send\(new MessageFollowProbeMsg\(/);
  assert.match(zoneClient, /\.request\(new MessageFollowProbeReq\(/);
  assert.match(zoneHandlers, /zlinkSpotActorRequestHandler\([\s\S]*?messageFollowProbeReq/);
  assert.match(zoneHandlers, /zlinkSpotActorSendHandler\([\s\S]*?messageFollowProbeMsg/);

  const supportActor = read(
    'samples/SupportChat.Ts/Server/Support/Infrastructure/ZLink/Actors/support-user-actor.ts'
  );
  const supportMessages = read('samples/SupportChat.Ts/Shared/Contracts/messages.ts');
  assert.match(supportActor, /class DeliverSupportNotificationMsg\b/);
  assert.doesNotMatch(supportActor, /class DeliverSupportNotification\b/);
  assert.doesNotMatch(supportActor, /class JoinSupportConversationHandler\b/);
  assert.match(supportMessages, /class ConversationCreateReq\b/);

  const shoppingClient = read('samples/ShoppingMall.Ts/Client/shoppingmall-client-scenario.ts');
  assert.match(shoppingClient, /type VersionFenceRes\b/);
  assert.doesNotMatch(shoppingClient, /VersionFenceResult/);
});

test('Actor and Spot creation payloads use request wrappers', () => {
  const ticTacToeMessages = read('samples/TicTacToe.Ts/Shared/Contracts/messages.ts');
  const ticTacToeSession = read(
    'samples/TicTacToe.Ts/Server/Play/Infrastructure/ZLink/Sessions/Handlers/authenticate-play-session-handler.ts'
  );
  const ticTacToeEntry = read(
    'samples/TicTacToe.Ts/Server/Play/Infrastructure/ZLink/Spots/EntrySpot/play-entry-spot.ts'
  );
  assert.match(ticTacToeMessages, /class PlayerActorCreateReq\b/);
  assert.match(ticTacToeSession, /\.request\(new PlayerActorCreateReq\(authenticated\.player\)\)/);
  assert.match(ticTacToeEntry, /createRequest\.decode\(PlayerActorCreateReq\)/);

  const bingoProto = read('samples/Bingo.Ts/Shared/Contracts/bingo_messages.proto');
  const bingoApi = read('samples/Bingo.Ts/Server/Api/Handlers/match-bingo-handler.ts');
  const bingoSession = read(
    'samples/Bingo.Ts/Server/Session/Sessions/Handlers/authenticate-session-handler.ts'
  );
  const bingoRoom = read(
    'samples/Bingo.Ts/Server/Play/Infrastructure/ZLink/Spots/BingoRoomSpot/bingo-room-spot.ts'
  );
  assert.match(bingoProto, /message PlayerActorCreateReq\b/);
  assert.match(bingoProto, /message BingoRoomCreateReq\b/);
  assert.doesNotMatch(bingoProto, /EnsurePlayerActorReq|EnsurePlayerActorRes/);
  assert.match(bingoSession, /\.request\(new PlayerActorCreateReq\(/);
  assert.match(bingoApi, /\.request\(new BingoRoomCreateReq\(/);
  assert.match(bingoRoom, /request\.decode<BingoRoomCreateReq>\(\)/);

  const deliveryMessages = read('samples/DeliveryDispatch.Ts/Shared/Contracts/messages.ts');
  const deliverySession = read('samples/DeliveryDispatch.Ts/Server/Session/customer-session.ts');
  assert.match(deliveryMessages, /class CustomerActorCreateReq\b/);
  assert.match(deliverySession, /\.request\(new CustomerActorCreateReq\(CustomerId\)\)/);
  assert.doesNotMatch(deliverySession, /\.request\(\{\s*customerId:/);
});

test('internal Actor delivery uses Msg wrappers before client Notify push', () => {
  const bingoProto = read('samples/Bingo.Ts/Shared/Contracts/bingo_messages.proto');
  const bingoActor = read(
    'samples/Bingo.Ts/Server/Play/Infrastructure/ZLink/Actors/player-actor.ts'
  );
  const bingoRoom = read(
    'samples/Bingo.Ts/Server/Play/Infrastructure/ZLink/Spots/BingoRoomSpot/bingo-room-spot.ts'
  );
  for (const type of [
    'DeliverPlayerJoinedMsg',
    'DeliverBingoGameStartedMsg',
    'DeliverBingoNumberDrawnMsg',
    'DeliverBingoGameEndedMsg',
    'DeliverBingoRewardAnnouncedMsg'
  ]) {
    assert.match(bingoProto, new RegExp(`message ${type}\\b`));
    assert.match(bingoActor, new RegExp(`packetName: '${type}'`));
    assert.match(bingoRoom, new RegExp(`new ${type}\\(`));
  }
  assert.match(bingoProto, /message LeaveFinishedBingoRoomMsg\b/);
  assert.match(bingoProto, /message DestroyBingoActorMsg\b/);
  assert.doesNotMatch(bingoProto, /message (?:LeaveFinishedBingoRoom|DestroyBingoActor)\s*\{/);

  const zoneActor = read(
    'samples/ZoneWorld/Server/ZoneNode/Infrastructure/ZLink/Actors/player-actor.ts'
  );
  const zoneSpot = read(
    'samples/ZoneWorld/Server/ZoneNode/Infrastructure/ZLink/Spots/zone-spot.ts'
  );
  assert.match(zoneActor, /class DeliverZoneNotificationMsg\b/);
  assert.match(zoneSpot, /sendToActor\(actorId, new DeliverZoneNotificationMsg\(payload\)\)/);
});

test('sample request handlers keep named Req and Res types at wire boundaries', () => {
  const gameQuestMessages = read('samples/GameQuest.Ts/Shared/Contracts/messages.ts');
  const gameQuestHandler = read(
    'samples/GameQuest.Ts/Server/QuestMission/Infrastructure/ZLink/Spots/PlayerQuestSpot/player-quest-spot-handlers.ts'
  );
  const gameQuestServer = read('samples/GameQuest.Ts/Server/GameApi/game-api-server.ts');
  assert.match(gameQuestMessages, /type RebuildQuestProjectionRes\b/);
  assert.match(gameQuestHandler, /RebuildQuestProjectionReq, RebuildQuestProjectionRes/);
  assert.match(gameQuestServer, /submit<RebuildQuestProjectionRes>\(\)/);

  const bingoHandlers = read(
    'samples/Bingo.Ts/Server/Play/Infrastructure/ZLink/Spots/BingoRoomSpot/Handlers/bingo-room-operation-handlers.ts'
  );
  assert.match(bingoHandlers, /type VerifyStopObservingAtSpotRes\b/);
  assert.doesNotMatch(bingoHandlers, /VerifyStopObservingAtSpotReq,\s*\{/);

  const shoppingContinue = read(
    'samples/ShoppingMall.Ts/Server/OrderWorkflow/Infrastructure/ZLink/Spots/OrderWorkflowSpot/Handlers/continue-workflow-handler.ts'
  );
  const shoppingRebuild = read(
    'samples/ShoppingMall.Ts/Server/OrderWorkflow/Infrastructure/ZLink/Spots/OrderWorkflowSpot/Handlers/rebuild-order-projection-handler.ts'
  );
  assert.match(shoppingContinue, /OrderWorkflowSpot, ContinueOrderWorkflowReq, ContinueOrderWorkflowRes/);
  assert.match(shoppingRebuild, /OrderWorkflowSpot, RebuildOrderProjectionReq, RebuildOrderProjectionRes/);
  assert.doesNotMatch(shoppingContinue, /OrderWorkflowSpot, \{ orderId: string \}/);
  assert.doesNotMatch(shoppingRebuild, /OrderWorkflowSpot, \{ orderId: string \}/);

  const zoneNode = read('samples/ZoneWorld/Server/ZoneNode/main.ts');
  assert.match(zoneNode, /submit<EnterWorldRes>\(\)/);
  assert.doesNotMatch(zoneNode, /submit<\{ error: string \| null \}>\(\)/);
});
