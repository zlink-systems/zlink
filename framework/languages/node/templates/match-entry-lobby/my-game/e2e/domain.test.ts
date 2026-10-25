import assert from 'node:assert/strict';
import { test } from 'node:test';
import { Game, Rejection } from '../server/shared/constants.js';
import {
  ActionRejected,
  MatchEnded,
  MatchFound,
  MoveRequest,
  PlayerView,
  QueueRequest,
  RoomAdmission,
  RoomAssignment,
  RoomView,
  RoomViewRequest,
  SessionInfo,
  SessionInfoRequest,
  ShootRequest,
  signedInt32,
  toRetryAfterMs,
  type Int32
} from '../server/shared/messages.js';
import { MatchmakingState, RoomState, type RoomClock } from '../server/match/domain.js';

class ManualClock implements RoomClock {
  constructor(private milliseconds: number) {}

  now(): number {
    return this.milliseconds;
  }

  advance(milliseconds: number): void {
    this.milliseconds += milliseconds;
  }
}

function newRoom(clock: ManualClock): RoomState {
  const room = new RoomState(clock);
  assert.equal(room.admit('a'), true);
  assert.equal(room.admit('b'), true);
  return room;
}

function positioned(clock: ManualClock, ax: number, ay: number, bx = 10, by = 10): RoomState {
  const room = newRoom(clock);
  const snapshot = JSON.parse(new TextDecoder().decode(room.capture())) as {
    Players: { X: number; Y: number }[];
  };
  snapshot.Players[0].X = ax;
  snapshot.Players[0].Y = ay;
  snapshot.Players[1].X = bx;
  snapshot.Players[1].Y = by;
  room.restore(new TextEncoder().encode(JSON.stringify(snapshot)));
  return room;
}

function player(room: RoomState, playerId: string) {
  const view = room.view().find((candidate) => candidate.playerId === playerId);
  assert(view, `Room view omitted player ${playerId}`);
  return view;
}

function rejection(result: ActionRejected | null, reason: string, retryAfterMs = 0): void {
  assert(result instanceof ActionRejected);
  assert.equal(result.reason, reason);
  assert.equal(result.retryAfterMs, retryAfterMs);
}

function wireValue(value: object): Record<string, unknown> {
  const encoded = JSON.stringify(value, (_key, item: unknown) =>
    typeof item === 'bigint' ? item.toString(10) : item
  );
  assert(encoded !== undefined);
  return JSON.parse(encoded) as Record<string, unknown>;
}

test('typed messages preserve the .NET typed JSON camelCase wire fields', () => {
  const messages: [string, object, string[]][] = [
    ['SessionInfoRequest', new SessionInfoRequest(), []],
    [
      'SessionInfo',
      new SessionInfo('session', 'actor', 1n),
      ['sessionId', 'actorId', 'objectGeneration']
    ],
    ['QueueRequest', new QueueRequest('player'), ['playerId', 'mode']],
    ['RoomAdmission', new RoomAdmission('player', 'room'), ['playerId', 'roomId']],
    ['RoomAssignment', new RoomAssignment('room'), ['roomId']],
    ['MatchFound', new MatchFound('room'), ['roomId']],
    ['MoveRequest', new MoveRequest(1, 2, 3), ['dx', 'dy', 'sequence']],
    ['ShootRequest', new ShootRequest('target', 4), ['targetId', 'sequence']],
    ['RoomViewRequest', new RoomViewRequest(), []],
    [
      'PlayerView',
      new PlayerView('player', 1, 2, 90, 5),
      ['playerId', 'x', 'y', 'hp', 'lastSequence']
    ],
    ['RoomView', new RoomView('room', 'node'), ['roomId', 'nodeId', 'phase', 'winner', 'players']],
    ['ActionRejected', new ActionRejected('reason'), ['reason', 'retryAfterMs']],
    ['MatchEnded', new MatchEnded('winner'), ['winner']]
  ];
  for (const [name, message, fields] of messages)
    assert.deepEqual(Object.keys(wireValue(message)), fields, name);

  const playing = wireValue(
    new RoomView('room', 'node', Game.playingPhase, null, [new PlayerView('player', 1, 2, 90, 5)])
  );
  assert.deepEqual(playing, {
    roomId: 'room',
    nodeId: 'node',
    phase: 'Playing',
    winner: null,
    players: [{ playerId: 'player', x: 1, y: 2, hp: 90, lastSequence: 5 }]
  });
  assert.equal(typeof playing.phase, 'string');
  const finished = wireValue(new RoomView('room', 'node', Game.finishedPhase, 'winner'));
  assert.equal(finished.phase, 'Finished');
});

test('RetryAfterMs is a signed int32 JSON number and generated delays clamp safely', () => {
  const minimum = signedInt32(-2_147_483_648);
  const maximum = signedInt32(2_147_483_647);
  assert.equal(minimum, -2_147_483_648);
  assert.equal(maximum, 2_147_483_647);
  assert.throws(() => signedInt32(-2_147_483_649), RangeError);
  assert.throws(() => signedInt32(2_147_483_648), RangeError);
  assert.throws(() => signedInt32(1.5), RangeError);

  assert.equal(toRetryAfterMs(Number.POSITIVE_INFINITY), maximum);
  assert.equal(toRetryAfterMs(Number.NEGATIVE_INFINITY), 0);
  assert.throws(() => toRetryAfterMs(Number.NaN), RangeError);
  assert.throws(() => new ActionRejected('invalid', 2_147_483_648 as Int32), RangeError);

  const rejection = wireValue(new ActionRejected('move-budget', toRetryAfterMs(Number.MAX_VALUE)));
  assert.deepEqual(rejection, { reason: 'move-budget', retryAfterMs: 2_147_483_647 });
  assert.equal(typeof rejection.retryAfterMs, 'number');
});

test('shared action sequences commit only accepted actions', () => {
  const room = newRoom(new ManualClock(0));
  assert.equal(room.move('a', new MoveRequest(0, 0, 1)), null);
  const before = room.capture();

  rejection(room.move('a', new MoveRequest(0, 0, 1)), Rejection.staleSequence);
  assert.deepEqual(room.capture(), before, 'duplicate sequence changed room state');
  rejection(room.shoot('a', new ShootRequest('b', 0)), Rejection.staleSequence);
  assert.deepEqual(room.capture(), before, 'reversed shared sequence changed room state');
  const overflowSequence = Game.maxSafeInteger + 1;
  room.players[0].lastSequence = overflowSequence;
  const beforeOverflow = room.capture();
  rejection(room.move('a', new MoveRequest(0, 0, overflowSequence)), Rejection.integerOutOfRange);
  assert.deepEqual(room.capture(), beforeOverflow, 'out-of-range move changed room state');
  rejection(
    room.shoot('a', new ShootRequest('b', overflowSequence)),
    Rejection.integerOutOfRange
  );
  assert.deepEqual(room.capture(), beforeOverflow, 'out-of-range shot changed room state');

  const matchmaking = new MatchmakingState();
  assert.equal(matchmaking.allocateRoom(), 'room-1');
  matchmaking.roomSequence = Game.maxSafeInteger;
  assert.equal(matchmaking.allocateRoom(), null, 'unsafe room sequence was allocated');
});

test('MaxStep and inclusive map bounds are enforced', () => {
  const stepRoom = newRoom(new ManualClock(0));
  assert.equal(stepRoom.move('a', new MoveRequest(Game.maxStep, 0, 1)), null);
  const before = stepRoom.capture();
  rejection(
    stepRoom.move('a', new MoveRequest(Game.maxStep + 0.001, 0, 2)),
    Rejection.stepTooLarge
  );
  assert.deepEqual(stepRoom.capture(), before);

  const edges = [
    { name: 'x lower inclusive', x: 2, y: 50, dx: -2, dy: 0, targetX: 0, targetY: 50 },
    { name: 'x upper inclusive', x: 98, y: 50, dx: 2, dy: 0, targetX: 100, targetY: 50 },
    { name: 'y lower inclusive', x: 50, y: 2, dx: 0, dy: -2, targetX: 50, targetY: 0 },
    { name: 'y upper inclusive', x: 50, y: 98, dx: 0, dy: 2, targetX: 50, targetY: 100 }
  ];
  for (const edge of edges) {
    const room = positioned(new ManualClock(0), edge.x, edge.y);
    assert.equal(room.move('a', new MoveRequest(edge.dx, edge.dy, 1)), null, edge.name);
    assert.equal(player(room, 'a').x, edge.targetX, edge.name);
    assert.equal(player(room, 'a').y, edge.targetY, edge.name);
  }

  const outside = [
    { name: 'x lower outside', x: 0, y: 50, dx: -0.01, dy: 0 },
    { name: 'x upper outside', x: 100, y: 50, dx: 0.01, dy: 0 },
    { name: 'y lower outside', x: 50, y: 0, dx: 0, dy: -0.01 },
    { name: 'y upper outside', x: 50, y: 100, dx: 0, dy: 0.01 }
  ];
  for (const edge of outside) {
    const room = positioned(new ManualClock(0), edge.x, edge.y);
    const before = room.capture();
    rejection(room.move('a', new MoveRequest(edge.dx, edge.dy, 1)), Rejection.outsideMap);
    assert.deepEqual(room.capture(), before, edge.name);
  }
});

test('closed wall endpoints and crossing block movement', () => {
  const clock = new ManualClock(0);
  for (const endpointY of [Game.wallMinY, Game.wallMaxY]) {
    const room = positioned(clock, Game.wallX - Game.maxStep, endpointY);
    const before = room.capture();
    rejection(room.move('a', new MoveRequest(Game.maxStep, 0, 1)), Rejection.moveBlockedByWall);
    assert.deepEqual(room.capture(), before, `wall endpoint ${endpointY} changed room state`);
  }

  const crossing = positioned(clock, Game.wallX - 1, (Game.wallMinY + Game.wallMaxY) / 2);
  const crossingBefore = crossing.capture();
  rejection(crossing.move('a', new MoveRequest(Game.maxStep, 0, 1)), Rejection.moveBlockedByWall);
  assert.deepEqual(crossing.capture(), crossingBefore, 'wall crossing changed room state');

  const clearLane = positioned(clock, Game.wallX - Game.maxStep, Game.wallMinY - 1);
  assert.equal(clearLane.move('a', new MoveRequest(Game.maxStep, 0, 1)), null);
});

test('movement grants a full first step, proportional elapsed budget, and a MaxStep cap', () => {
  const clock = new ManualClock(0);
  const room = newRoom(clock);
  assert.equal(room.move('a', new MoveRequest(Game.maxStep, 0, 1)), null);
  clock.advance(100);
  assert.equal(room.move('a', new MoveRequest(1, 0, 2)), null);

  const before = room.capture();
  rejection(room.move('a', new MoveRequest(0.1, 0, 3)), Rejection.moveBudget, 10);
  assert.deepEqual(room.capture(), before, 'movement rejection changed sequence or budget');
  clock.advance(10);
  assert.equal(room.move('a', new MoveRequest(0.1, 0, 3)), null);
  clock.advance(1000);
  assert.equal(room.move('a', new MoveRequest(Game.maxStep, 0, 4)), null);
});

test('nonfinite move coordinates receive invalid-move without changing state', () => {
  for (const value of [Number.NaN, Number.POSITIVE_INFINITY, Number.NEGATIVE_INFINITY]) {
    const room = newRoom(new ManualClock(0));
    const before = room.capture();
    rejection(room.move('a', new MoveRequest(value, 0, 1)), Rejection.invalidMove);
    assert.deepEqual(room.capture(), before, `nonfinite move ${value} changed room state`);
  }
});

test('fire interval rejects early shots and accepts the exact boundary', () => {
  const clock = new ManualClock(0);
  const room = newRoom(clock);
  assert.equal(room.shoot('a', new ShootRequest('b', 1)), null);
  const before = room.capture();
  rejection(room.shoot('a', new ShootRequest('b', 2)), Rejection.fireTooSoon, Game.fireIntervalMs);
  assert.deepEqual(room.capture(), before);
  clock.advance(Game.fireIntervalMs - 1);
  const lastMillisecondBefore = room.capture();
  rejection(room.shoot('a', new ShootRequest('b', 2)), Rejection.fireTooSoon, 1);
  assert.deepEqual(room.capture(), lastMillisecondBefore);
  clock.advance(1);
  assert.equal(room.shoot('a', new ShootRequest('b', 2)), null);
});

test('relocation snapshot stores remaining milliseconds and restores under another clock origin', () => {
  const sourceClock = new ManualClock(5_000_000);
  const source = newRoom(sourceClock);
  assert.equal(source.move('a', new MoveRequest(Game.maxStep, 0, 1)), null);
  assert.equal(source.shoot('a', new ShootRequest('b', 2)), null);
  sourceClock.advance(60);
  const snapshot = source.capture();
  const text = new TextDecoder().decode(snapshot);
  const parsed = JSON.parse(text) as {
    Players: { MoveBudgetRemainingMs: number | null; FireCooldownRemainingMs: number | null }[];
  };
  assert.equal(parsed.Players[0].MoveBudgetRemainingMs, 140);
  assert.equal(parsed.Players[0].FireCooldownRemainingMs, 190);
  assert(!text.includes('Timestamp'), 'Snapshot stored an absolute timestamp');
  assert.deepEqual(snapshot, source.capture(), 'Capture changed at a fixed domain clock time');

  const destinationClock = new ManualClock(900_000_000);
  const destination = new RoomState(destinationClock);
  destination.restore(snapshot);
  assert.deepEqual(destination.view(), source.view());
  assert.equal(destination.Phase, source.Phase);
  assert.equal(destination.Winner, source.Winner);
  assert.equal(player(destination, 'a').lastSequence, 2);
  rejection(destination.move('a', new MoveRequest(Game.maxStep, 0, 3)), Rejection.moveBudget, 140);
  rejection(destination.shoot('a', new ShootRequest('b', 3)), Rejection.fireTooSoon, 190);
  destinationClock.advance(140);
  assert.equal(destination.move('a', new MoveRequest(Game.maxStep, 0, 3)), null);
  destinationClock.advance(50);
  assert.equal(destination.shoot('a', new ShootRequest('b', 4)), null);
});

test('finished rooms reject movement and shooting without changing the snapshot', () => {
  const clock = new ManualClock(0);
  const room = newRoom(clock);
  for (let sequence = 1; sequence <= Game.initialHp / Game.shotDamage; sequence++) {
    assert.equal(room.shoot('a', new ShootRequest('b', sequence)), null);
    clock.advance(Game.fireIntervalMs);
  }
  assert.equal(room.Phase, Game.finishedPhase);
  assert.equal(room.Winner, 'a');
  assert.equal(player(room, 'b').hp, 0);

  const before = room.capture();
  rejection(room.move('a', new MoveRequest(0, 0, 5)), Rejection.matchFinished);
  assert.deepEqual(room.capture(), before, 'finished movement rejection changed room state');
  rejection(room.shoot('a', new ShootRequest('b', 5)), Rejection.matchFinished);
  assert.deepEqual(room.capture(), before, 'finished shooting rejection changed room state');
});
