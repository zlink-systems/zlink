const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const test = require('node:test');

const root = path.resolve(__dirname, '../..');

test('TicTacToe uses one-way client join and a distinct internal room join response', () => {
  const messages = fs.readFileSync(path.join(root, 'samples/TicTacToe.Ts/Shared/Contracts/messages.ts'), 'utf8');
  const room = fs.readFileSync(path.join(
    root,
    'samples/TicTacToe.Ts/Server/Play/Infrastructure/ZLink/Spots/TicTacToeGameSpot/tictactoe-game-spot.ts'
  ), 'utf8');
  const joinHandler = fs.readFileSync(path.join(
    root,
    'samples/TicTacToe.Ts/Server/Play/Infrastructure/ZLink/Spots/EntrySpot/Handlers/play-actor-join-game-handler.ts'
  ), 'utf8');
  const actor = fs.readFileSync(path.join(
    root,
    'samples/TicTacToe.Ts/Server/Play/Infrastructure/ZLink/Actors/play-actor.ts'
  ), 'utf8');
  const client = fs.readFileSync(path.join(
    root,
    'samples/TicTacToe.Ts/Client/tictactoe-client-scenario.ts'
  ), 'utf8');

  assert.match(messages, /interface TicTacToeGameJoinRes \{[\s\S]*?state: GameState;/);
  assert.match(messages, /class JoinGameMsg \{[\s\S]*?roomId: string/);
  assert.match(messages, /class JoinGameNotify \{[\s\S]*?state: GameState/);
  assert.match(messages, /class JoinGameFailedNotify \{[\s\S]*?roomId: string,[\s\S]*?error: string/);
  assert.doesNotMatch(messages, /JoinGameReq|JoinGameRes|joinGameReq|joinGameRes/);
  assert.match(messages, /class LeaveGameMsg \{[\s\S]*?roomId: string/);
  assert.doesNotMatch(messages, /LeaveGameReq|leaveGameReq/);
  assert.match(joinHandler, /ZLinkEntrySpotActorSendHandler<[\s\S]*?JoinGameMsg>/);
  assert.match(joinHandler, /\.joinSpot\(message\.roomId, joinRequest\)[\s\S]*?\.defer\(\)/);
  assert.doesNotMatch(joinHandler, /return joinGame/);
  assert.match(room, /private admit\([^)]*\): TicTacToeGameJoinRes/);
  assert.match(actor, /completion\.status === 'rejected'[\s\S]*?new JoinGameFailedNotify/);
  assert.match(actor, /completion\.status === 'failed'[\s\S]*?new JoinGameFailedNotify/);
  assert.match(actor, /joinGameNotify\(joined\.state\)/);
  assert.match(client, /waitFor<JoinGameNotify>\(PacketNames\.joinGameNotify\)[\s\S]*?send\(new JoinGameMsg\(game\.roomId\)\)/);
});

test('TicTacToe wire names describe the framework call flow', () => {
  const sampleRoot = path.join(root, 'samples/TicTacToe.Ts');
  const messages = fs.readFileSync(path.join(sampleRoot, 'Shared/Contracts/messages.ts'), 'utf8');
  const room = fs.readFileSync(path.join(
    sampleRoot,
    'Server/Play/Infrastructure/ZLink/Spots/TicTacToeGameSpot/tictactoe-game-spot.ts'
  ), 'utf8');
  const actor = fs.readFileSync(path.join(
    sampleRoot,
    'Server/Play/Infrastructure/ZLink/Actors/play-actor.ts'
  ), 'utf8');
  const registry = fs.readFileSync(path.join(
    sampleRoot,
    'Server/Play/Infrastructure/ZLink/Spots/EntrySpot/entry-spot-registries.ts'
  ), 'utf8');

  assert.doesNotMatch(messages, /(?:class|interface|type)\s+\w+(?:Command|Result|Ack)\b/);
  assert.doesNotMatch(messages, /JoinGameReq|JoinGameRes|joinGameReq|joinGameRes/);
  const packetNames = [...messages.matchAll(/:\s*'([A-Za-z0-9]+(?:Req|Res|Msg|Notify|Event))'/g)]
    .map((match) => match[1]);
  assert.deepEqual(packetNames, [
    'AuthenticateReq',
    'AuthenticateRes',
    'AuthenticatePlayerReq',
    'AuthenticatePlayerRes',
    'CreateGameHttpReq',
    'CreateGameHttpRes',
    'JoinGameMsg',
    'JoinGameNotify',
    'JoinGameFailedNotify',
    'ObserveMilestoneReq',
    'ObserveMilestoneRes',
    'PlaceMarkReq',
    'PlaceMarkRes',
    'LeaveGameMsg',
    'DeliverPlayNotificationMsg',
    'PlayerJoinedNotify',
    'GameStateNotify',
    'WinMilestoneNotify',
    'PlayerWinMilestoneEvent'
  ]);
  assert.match(actor, /ZLinkSpotActorSend\(PacketNames\.deliverPlayNotificationMsg\)/);
  assert.match(registry, /sendToActor\(actorId, new DeliverPlayNotificationMsg\(payload\)\)/);
  assert.match(room, /\.publish\([\s\S]*?playerWinMilestoneEvent\(/);
  assert.equal((messages.match(/:\s*'\w+Event'/g) ?? []).length, 1);
  assert.match(messages, /playerWinMilestoneEvent:\s*'PlayerWinMilestoneEvent'/);
});
