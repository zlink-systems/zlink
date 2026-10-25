import { Names } from '../server/shared/constants.js';
import { MatchRequest, MatchResponse, PrepareRoom } from '../server/shared/messages.js';
import { Matchmaker } from '../server/lobby/matchmaker.js';
import { RoomRoster } from '../server/match/roster.js';
import { encodeRoomSnapshot, decodeRoomSnapshot } from '../server/match/snapshot.js';
import type { RoomSnapshot } from '../server/match/domain.js';
function manualClock(start = 0) { return new ManualClock(start); }
import assert from 'node:assert/strict';
import { Game, Rejection } from '../server/shared/constants.js';
import {
  ActionRejected,
  MoveRequest,
  ShootRequest,
} from '../server/shared/messages.js';
import { RoomState, type RoomClock } from '../server/match/domain.js';

const cases: { name: string; run(): void }[] = [];
function test(name: string, run: () => void): void { cases.push({ name, run }); }

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

test('shared action sequences commit only accepted actions', () => {
  const room = newRoom(new ManualClock(0));
  assert.equal(room.move('a', new MoveRequest(0, 0, BigInt(1))), null);
  const before = room.capture();

  rejection(room.move('a', new MoveRequest(0, 0, BigInt(1))), Rejection.staleSequence);
  assert.deepEqual(room.capture(), before, 'duplicate sequence changed room state');
  rejection(room.shoot('a', new ShootRequest('b', BigInt(0))), Rejection.staleSequence);
  assert.deepEqual(room.capture(), before, 'reversed shared sequence changed room state');
  const overflowSequence = BigInt(Game.maxSafeInteger) + 1n;
  room.players[0].lastSequence = overflowSequence;
  const beforeOverflow = room.capture();
  rejection(room.move('a', new MoveRequest(0, 0, BigInt(overflowSequence))), Rejection.integerOutOfRange);
  assert.deepEqual(room.capture(), beforeOverflow, 'out-of-range move changed room state');
  rejection(
    room.shoot('a', new ShootRequest('b', BigInt(overflowSequence))),
    Rejection.integerOutOfRange
  );
  assert.deepEqual(room.capture(), beforeOverflow, 'out-of-range shot changed room state');

});

test('MaxStep and inclusive map bounds are enforced', () => {
  const stepRoom = newRoom(new ManualClock(0));
  assert.equal(stepRoom.move('a', new MoveRequest(Game.maxStep, 0, BigInt(1))), null);
  const before = stepRoom.capture();
  rejection(
    stepRoom.move('a', new MoveRequest(Game.maxStep + 0.001, 0, BigInt(2))),
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
    assert.equal(room.move('a', new MoveRequest(edge.dx, edge.dy, BigInt(1))), null, edge.name);
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
    rejection(room.move('a', new MoveRequest(edge.dx, edge.dy, BigInt(1))), Rejection.outsideMap);
    assert.deepEqual(room.capture(), before, edge.name);
  }
});

test('closed wall endpoints and crossing block movement', () => {
  const clock = new ManualClock(0);
  for (const endpointY of [Game.wallMinY, Game.wallMaxY]) {
    const room = positioned(clock, Game.wallX - Game.maxStep, endpointY);
    const before = room.capture();
    rejection(room.move('a', new MoveRequest(Game.maxStep, 0, BigInt(1))), Rejection.moveBlockedByWall);
    assert.deepEqual(room.capture(), before, `wall endpoint ${endpointY} changed room state`);
  }

  const crossing = positioned(clock, Game.wallX - 1, (Game.wallMinY + Game.wallMaxY) / 2);
  const crossingBefore = crossing.capture();
  rejection(crossing.move('a', new MoveRequest(Game.maxStep, 0, BigInt(1))), Rejection.moveBlockedByWall);
  assert.deepEqual(crossing.capture(), crossingBefore, 'wall crossing changed room state');

  const clearLane = positioned(clock, Game.wallX - Game.maxStep, Game.wallMinY - 1);
  assert.equal(clearLane.move('a', new MoveRequest(Game.maxStep, 0, BigInt(1))), null);
});

test('movement grants a full first step, proportional elapsed budget, and a MaxStep cap', () => {
  const clock = new ManualClock(0);
  const room = newRoom(clock);
  assert.equal(room.move('a', new MoveRequest(Game.maxStep, 0, BigInt(1))), null);
  clock.advance(100);
  assert.equal(room.move('a', new MoveRequest(1, 0, BigInt(2))), null);

  const before = room.capture();
  rejection(room.move('a', new MoveRequest(0.1, 0, BigInt(3))), Rejection.moveBudget, 10);
  assert.deepEqual(room.capture(), before, 'movement rejection changed sequence or budget');
  clock.advance(10);
  assert.equal(room.move('a', new MoveRequest(0.1, 0, BigInt(3))), null);
  clock.advance(1000);
  assert.equal(room.move('a', new MoveRequest(Game.maxStep, 0, BigInt(4))), null);
});

test('nonfinite move coordinates receive invalid-move without changing state', () => {
  for (const value of [Number.NaN, Number.POSITIVE_INFINITY, Number.NEGATIVE_INFINITY]) {
    const room = newRoom(new ManualClock(0));
    const before = room.capture();
    rejection(room.move('a', new MoveRequest(value, 0, BigInt(1))), Rejection.invalidMove);
    assert.deepEqual(room.capture(), before, `nonfinite move ${value} changed room state`);
  }
});

test('shot validation rejects wall, range, and missing targets without mutation', () => {
  for (const [room, target, expected] of [
    [positioned(new ManualClock(0), 40, 40, 60, 40), 'b', Rejection.blockedByWall],
    [positioned(new ManualClock(0), 10, 10, 95, 10), 'b', Rejection.outOfRange],
    [newRoom(new ManualClock(0)), 'missing', Rejection.noTarget]
  ] as const) {
    const before = room.capture();
    rejection(room.shoot('a', new ShootRequest(target, 1n)), expected);
    assert.deepEqual(room.capture(), before, `${expected} changed room state`);
  }
});

test('fire interval rejects early shots and accepts the exact boundary', () => {
  const clock = new ManualClock(0);
  const room = newRoom(clock);
  assert.equal(room.shoot('a', new ShootRequest('b', BigInt(1))), null);
  const before = room.capture();
  rejection(room.shoot('a', new ShootRequest('b', BigInt(2))), Rejection.fireTooSoon, Game.fireIntervalMs);
  assert.deepEqual(room.capture(), before);
  clock.advance(Game.fireIntervalMs - 1);
  const lastMillisecondBefore = room.capture();
  rejection(room.shoot('a', new ShootRequest('b', BigInt(2))), Rejection.fireTooSoon, 1);
  assert.deepEqual(room.capture(), lastMillisecondBefore);
  clock.advance(1);
  assert.equal(room.shoot('a', new ShootRequest('b', BigInt(2))), null);
});

test('relocation snapshot stores remaining milliseconds and restores under another clock origin', () => {
  const sourceClock = new ManualClock(5_000_000);
  const source = newRoom(sourceClock);
  assert.equal(source.move('a', new MoveRequest(Game.maxStep, 0, BigInt(1))), null);
  assert.equal(source.shoot('a', new ShootRequest('b', BigInt(2))), null);
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
  assert.equal(player(destination, 'a').lastSequence, 2n);
  rejection(destination.move('a', new MoveRequest(Game.maxStep, 0, BigInt(3))), Rejection.moveBudget, 140);
  rejection(destination.shoot('a', new ShootRequest('b', BigInt(3))), Rejection.fireTooSoon, 190);
  destinationClock.advance(140);
  assert.equal(destination.move('a', new MoveRequest(Game.maxStep, 0, BigInt(3))), null);
  destinationClock.advance(50);
  assert.equal(destination.shoot('a', new ShootRequest('b', BigInt(4))), null);
});

test('finished rooms reject movement and shooting without changing the snapshot', () => {
  const clock = new ManualClock(0);
  const room = newRoom(clock);
  for (let sequence = 1; sequence <= Game.initialHp / Game.shotDamage; sequence++) {
    assert.equal(room.shoot('a', new ShootRequest('b', BigInt(sequence))), null);
    clock.advance(Game.fireIntervalMs);
  }
  assert.equal(room.Phase, Game.finishedPhase);
  assert.equal(room.Winner, 'a');
  assert.equal(player(room, 'b').hp, 0);

  const before = room.capture();
  rejection(room.move('a', new MoveRequest(0, 0, BigInt(5))), Rejection.matchFinished);
  assert.deepEqual(room.capture(), before, 'finished movement rejection changed room state');
  rejection(room.shoot('a', new ShootRequest('b', BigInt(5))), Rejection.matchFinished);
  assert.deepEqual(room.capture(), before, 'finished shooting rejection changed room state');
});

export function runDomainChecks(): void {
  for (const entry of cases) entry.run();
  assert.equal(cases.length, 9);
  console.log(`Game domain checks: ${cases.length}/${cases.length} categories passed`);
}
export async function runHttpDomainChecks(): Promise<void> {
  let checkedCount = 0;
  const check = (condition: boolean, name: string): void => {
    assert(condition, `HTTP domain boundary failed: ${name}`);
    checkedCount++;
  };
  const rejects = (action: () => void): boolean => {
    try {
      action();
      return false;
    } catch {
      return true;
    }
  };
  const roster = new RoomRoster();
  check(!roster.canAdmit('a') && !roster.canAdmit('outsider'), 'unprepared Room rejects admission');
  check(rejects(() => roster.prepare('room', new PrepareRoom('other', ['a', 'b']))), 'wrong match identity');
  check(rejects(() => roster.prepare('room', new PrepareRoom('room', ['a', 'a']))), 'duplicate roster member');
  check(rejects(() => roster.prepare('room', new PrepareRoom('room', ['a']))), 'incomplete roster');

  const members = ['a', 'b'];
  check(roster.prepare('room', new PrepareRoom('room', members)) === 'room', 'preparation acknowledges identity');
  members[0] = 'outsider';
  check(
    roster.canAdmit('a') && roster.canAdmit('b') && !roster.canAdmit('outsider'),
    'RoomRoster owns a copy of its member list'
  );
  const rosterSnapshot = roster.capture();
  roster.prepare('room', new PrepareRoom('room', ['b', 'a']));
  check(equalSnapshot(rosterSnapshot, roster.capture()), 'same preparation is idempotent');
  check(
    rejects(() => roster.prepare('room', new PrepareRoom('room', ['a', 'outsider']))) &&
      equalSnapshot(rosterSnapshot, roster.capture()),
    'prepared roster cannot be replaced'
  );

  const gameClock = manualClock(),
    game = new RoomState(gameClock);
  game.admit('a');
  game.admit('b');
  game.move('a', new MoveRequest(1, 0, 1n));
  game.shoot('a', new ShootRequest('b', 2n));
  const envelope = { Game: JSON.parse(new TextDecoder().decode(game.capture())) as RoomSnapshot, Roster: roster.capture() },
    restored = decodeRoomSnapshot<{
      Game: RoomSnapshot;
      Roster: ReturnType<RoomRoster['capture']>;
    }>(encodeRoomSnapshot(envelope)),
    restoredRoster = new RoomRoster(),
    restoredGame = new RoomState(manualClock(900_000));
  restoredRoster.restore(restored.Roster);
  restoredGame.restore(new TextEncoder().encode(JSON.stringify(restored.Game)));
  check(
    Object.keys(restored).join(',') === 'Game,Roster' &&
      restoredRoster.canAdmit('a') &&
      restoredRoster.canAdmit('b') &&
      !restoredRoster.canAdmit('outsider') &&
      equalSnapshot(game.view(), restoredGame.view()) &&
      game.Phase === restoredGame.Phase,
    'relocation envelope retains roster, state and sequence'
  );
  check(
    rejects(() => restoredRoster.prepare('room', new PrepareRoom('room', ['a', 'outsider']))),
    'relocated roster cannot be replaced'
  );
  check(
    rejects(() => new RoomRoster().restore({ matchId: null, members: ['a'] })),
    'unprepared snapshot with members'
  );

  const omitted = JSON.parse('{"playerId":"default"}') as { playerId: string; mode?: string };
  check(
    new MatchRequest(omitted.playerId, omitted.mode).mode === Names.defaultMode,
    'omitted HTTP mode defaults to duel'
  );

  type Call = { roomId: string; members: string[] };
  const calls: Call[] = [],
    endpoint = 'ws://127.0.0.1:1',
    provision = async (roomId: string, matched: string[]): Promise<MatchResponse> => {
      calls.push({ roomId, members: [...matched] });
      return new MatchResponse(roomId, endpoint);
    },
    firstProcess = new Matchmaker(provision),
    signal = new AbortController().signal,
    rankedA = firstProcess.match(new MatchRequest('ra', 'ranked'), signal),
    casualA = firstProcess.match(new MatchRequest('ca', 'casual'), signal),
    rankedB = firstProcess.match(new MatchRequest('rb', 'ranked'), signal);
  let casualSettled = false;
  void casualA.then(
    () => {
      casualSettled = true;
    },
    () => {
      casualSettled = true;
    }
  );
  await Promise.resolve();
  const [rankedFirst, rankedSecond] = await Promise.all([rankedA, rankedB]);
  check(!casualSettled && rankedFirst.roomId === rankedSecond.roomId, 'interleaved modes pair only matching members');
  const casualB = firstProcess.match(new MatchRequest('cb', 'casual'), signal),
    [casualFirst, casualSecond] = await Promise.all([casualA, casualB]);
  check(
    casualFirst.roomId === casualSecond.roomId && casualFirst.roomId !== rankedFirst.roomId,
    'different modes receive distinct Rooms'
  );
  const sameMembers = (a: string[], b: string[]): boolean =>
    a.length === b.length && a.every((member) => b.includes(member));
  check(
    sameMembers(calls[0].members, ['ra', 'rb']) && sameMembers(calls[1].members, ['ca', 'cb']),
    'provisioner receives each matched roster'
  );

  const restarted = new Matchmaker(provision),
    newA = restarted.match(new MatchRequest('new-a'), signal),
    newB = restarted.match(new MatchRequest('new-b'), signal),
    newRoom = await newA;
  check(
    equalSnapshot(newRoom, await newB) &&
      newRoom.roomId !== rankedFirst.roomId &&
      newRoom.roomId !== casualFirst.roomId,
    'restarted lobby creates a new Room ID'
  );
  check(
    calls.every((call) => /^room-[0-9a-f]{32}$/.test(call.roomId)),
    'Room ID uses a hyphen-free UUID'
  );

  let release!: (value: MatchResponse) => void;
  const completion = new Promise<MatchResponse>((resolve) => {
    release = resolve;
  });
  const heldCalls: Call[] = [],
    heldMatcher = new Matchmaker(async (roomId, matched) => {
      heldCalls.push({ roomId, members: [...matched] });
      return completion;
    }),
    heldA = heldMatcher.match(new MatchRequest('held-a'), signal),
    heldB = heldMatcher.match(new MatchRequest('held-b'), signal);
  let heldACompleted = false,
    heldBCompleted = false;
  void heldA.then(() => (heldACompleted = true));
  void heldB.then(() => (heldBCompleted = true));
  await Promise.resolve();
  check(
    !heldACompleted && !heldBCompleted && heldCalls.length === 1,
    'HTTP assignments wait for RoomPrepared'
  );
  release(new MatchResponse(heldCalls[0].roomId, endpoint));
  check(equalSnapshot(await heldA, await heldB), 'preparation releases both HTTP responses');

  assert.equal(checkedCount, 19);
  console.log(`HTTP domain boundaries: ${checkedCount}/${checkedCount} passed`);
}

function equalSnapshot(a: unknown, b: unknown): boolean {
  try {
    assert.deepEqual(a, b);
    return true;
  } catch {
    return false;
  }
}
