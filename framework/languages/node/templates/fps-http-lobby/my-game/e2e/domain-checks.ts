import assert from 'node:assert/strict';
import { Game, MatchPhase, Rejection, ShotOutcome } from '../server/shared/constants';
import { Aabb, ArenaMap, normalizeVec, Vec3, vecLength } from '../server/shared/geometry';
import { RoomState, type RoomSnapshot } from '../server/match/domain';
import { MoveRequest, ShootRequest } from '../server/shared/messages';

export function runDomainChecks(): void {
  let checkedCount = 0;
  const check = (condition: boolean, name: string): void => {
    assert(condition, `Domain boundary failed: ${name}`);
    checkedCount++;
  };
  const equal = (a: unknown, b: unknown): boolean => JSON.stringify(a) === JSON.stringify(b);
  const box = new Aabb(new Vec3(1, 1, 1), new Vec3(2, 2, 2));
  check(box.intersect(new Vec3(0, 1, 1), new Vec3(1, 0, 0), 1) === 1, 'closed face and exact limit');
  check(box.intersect(new Vec3(0, 2, 2), new Vec3(1, 0, 0), 3) === 1, 'parallel corner tangent');
  check(box.intersect(new Vec3(0, 2.001, 2), new Vec3(1, 0, 0), 3) === null, 'parallel outside slab');
  check(box.intersect(new Vec3(1.5, 1.5, 1.5), new Vec3(1, 0, 0), 3) === 0, 'inside origin');
  check(box.intersect(new Vec3(3, 1, 1), new Vec3(-1, 0, 0), 1) === 1, 'negative direction');
  check(box.intersect(new Vec3(0, 1, 1), new Vec3(-1, 0, 0), 3) === null, 'box behind ray');
  check(box.intersect(new Vec3(0, 1, 1), new Vec3(1, 0, 0), 0.999) === null, 'range cutoff');
  check(vecLength(new Vec3(Number.MIN_VALUE, 0, 0)) === Number.MIN_VALUE, 'subnormal movement length');
  const huge = normalizeVec(new Vec3(Number.MAX_VALUE, Number.MAX_VALUE, 0));
  check(huge !== null && Math.abs(vecLength(huge) - 1) < 1e-12, 'large finite aim normalization');
  const tiny = normalizeVec(new Vec3(Number.MIN_VALUE, 0, 0));
  check(tiny !== null && equal(tiny, new Vec3(1, 0, 0)), 'subnormal aim normalization');
  check(
    normalizeVec(new Vec3(Number.NaN, 0, 0)) === null &&
      normalizeVec(new Vec3(Number.POSITIVE_INFINITY, 0, 0)) === null &&
      normalizeVec(new Vec3(0, 0, 0)) === null,
    'invalid aim'
  );
  for (const spawn of ArenaMap.spawns)
    check(
      ArenaMap.bounds.contains(spawn) && !ArenaMap.walls.some((wall) => wall.contains(spawn)),
      'spawn outside walls and inside map'
    );

  let now = 1000;
  const room = new RoomState(() => now);
  room.admit('a');
  room.admit('b');
  check(room.move('a', new MoveRequest(new Vec3(0, 0, -2), 1)) === null, 'first movement full budget');
  let before = room.capture();
  const budget = room.move('a', new MoveRequest(new Vec3(0, 0, -2), 2));
  check(
    budget?.reason === Rejection.moveBudget && budget.retryAfterMs === 200 && equal(before, room.capture()),
    'movement rejection leaves timing and sequence unchanged'
  );
  const tinyMove = room.move('a', new MoveRequest(new Vec3(Number.MIN_VALUE, 0, 0), 2));
  check(
    tinyMove?.reason === Rejection.moveBudget && tinyMove.retryAfterMs > 0 && equal(before, room.capture()),
    'nonzero subnormal movement with zero elapsed budget leaves state unchanged'
  );
  now += 100;
  check(room.move('a', new MoveRequest(new Vec3(0, 0, -1), 2)) === null, 'partial elapsed movement budget');
  before = room.capture();
  check(
    room.move('a', new MoveRequest(new Vec3(0, 0, -3), 3))?.reason === Rejection.stepTooLarge &&
      equal(before, room.capture()),
    'oversize rejection invariant'
  );
  check(
    room.move('a', new MoveRequest(new Vec3(0, 1, 0), 3))?.reason === Rejection.verticalMove &&
      equal(before, room.capture()),
    'upward movement rejection invariant'
  );
  check(
    room.move('a', new MoveRequest(new Vec3(0, -1, 0), 3))?.reason === Rejection.verticalMove &&
      equal(before, room.capture()),
    'downward movement rejection invariant'
  );
  const edge = positioned(new Vec3(0, 0, 65), new Vec3(80, 0, 65));
  const edgeBefore = edge.capture();
  check(
    edge.move('a', new MoveRequest(new Vec3(-1, 0, 0), 1))?.reason === Rejection.outsideMap &&
      equal(edgeBefore, edge.capture()),
    'horizontal map boundary invariant'
  );
  check(
    room.move('a', new MoveRequest(new Vec3(Number.NaN, 0, 0), 3))?.reason === Rejection.nonFinite &&
      equal(before, room.capture()),
    'nonfinite movement invariant'
  );
  check(
    room.shoot('a', new ShootRequest(new Vec3(0, 1, 0), 3)).result?.outcome === ShotOutcome.miss,
    'miss is accepted'
  );
  before = room.capture();
  check(
    room.shoot('a', new ShootRequest(new Vec3(1, 0, 0), 3)).rejected?.reason === Rejection.staleSequence &&
      equal(before, room.capture()),
    'shared stale sequence invariant'
  );
  check(
    room.shoot('a', new ShootRequest(new Vec3(0, 0, 0), 4)).rejected?.reason === Rejection.invalidAim &&
      equal(before, room.capture()),
    'invalid aim leaves cooldown unchanged'
  );
  const fire = room.shoot('a', new ShootRequest(new Vec3(0, 1, 0), 4)).rejected;
  check(
    fire?.reason === Rejection.fireTooSoon && fire.retryAfterMs === Game.fireIntervalMs &&
      equal(before, room.capture()),
    'fire interval invariant'
  );
  now += Game.fireIntervalMs;
  check(room.shoot('a', new ShootRequest(new Vec3(0, 1, 0), 4)).result !== null, 'exact fire interval');
  check(room.move('a', new MoveRequest(new Vec3(0, 0, -2), 5)) === null, 'movement budget capped at max step');

  const tie = positioned(new Vec3(48, 0, 22), new Vec3(49.5, 0, 22));
  check(
    tie.shoot('a', new ShootRequest(new Vec3(1, 0, 0), 1)).result?.outcome === ShotOutcome.blockedByWall &&
      tie.view()[1].hp === Game.initialHp,
    'equal wall and player distance selects wall'
  );
  const behind = positioned(ArenaMap.spawns[0], ArenaMap.spawns[1]);
  check(
    behind.shoot('a', new ShootRequest(new Vec3(1, 0, 0), 1)).result?.outcome === ShotOutcome.hit &&
      behind.view()[1].hp === Game.initialHp - Game.shotDamage,
    'wall behind target does not block'
  );
  const exactRange = positioned(new Vec3(0, 0, 65), new Vec3(80.5, 0, 65));
  check(
    exactRange.shoot('a', new ShootRequest(new Vec3(1, 0, 0), 1)).result?.outcome === ShotOutcome.hit,
    'player face at exact range'
  );
  const beyondRange = positioned(new Vec3(0, 0, 65), new Vec3(80.501, 0, 65));
  check(
    beyondRange.shoot('a', new ShootRequest(new Vec3(1, 0, 0), 1)).result?.outcome === ShotOutcome.miss,
    'player face beyond range'
  );
  const crossing = positioned(new Vec3(48, 0, 22), ArenaMap.spawns[1]);
  before = crossing.capture();
  check(
    crossing.move('a', new MoveRequest(new Vec3(2, 0, 0), 1))?.reason === Rejection.moveBlockedByWall &&
      equal(before, crossing.capture()),
    'movement endpoint inside wall'
  );
  const touching = positioned(new Vec3(48, 0, 20), ArenaMap.spawns[1]);
  check(
    touching.move('a', new MoveRequest(new Vec3(1, 0, 0), 1))?.reason === Rejection.moveBlockedByWall,
    'movement touches wall face'
  );

  now = 1000;
  const source = new RoomState(() => now);
  source.admit('a');
  source.admit('b');
  source.move('a', new MoveRequest(new Vec3(0, 0, -2), 1));
  source.shoot('a', new ShootRequest(new Vec3(0, 1, 0), 2));
  now += 60;
  const snapshot = source.capture();
  check(
    snapshot.players[0].fireRemainingMs === 190 && snapshot.players[0].moveRemainingMs === 140,
    'snapshot stores remaining milliseconds'
  );
  check(equal(snapshot, source.capture()), 'capture does not mutate live state');
  let destinationNow = 900000;
  const destination = new RoomState(() => destinationNow);
  destination.restore(JSON.parse(JSON.stringify(snapshot)) as RoomSnapshot);
  check(
    equal(source.view(), destination.view()) && source.phase === destination.phase,
    'relocation keeps view under new clock origin'
  );
  const relocatedMove = destination.move('a', new MoveRequest(new Vec3(0, 0, -2), 3));
  check(
    relocatedMove?.reason === Rejection.moveBudget && relocatedMove.retryAfterMs === 140,
    'relocated movement budget'
  );
  const relocatedFire = destination.shoot('a', new ShootRequest(new Vec3(0, 1, 0), 3)).rejected;
  check(
    relocatedFire?.reason === Rejection.fireTooSoon && relocatedFire.retryAfterMs === 190,
    'relocated fire cooldown'
  );
  destinationNow += 140;
  check(
    destination.move('a', new MoveRequest(new Vec3(0, 0, -2), 3)) === null,
    'relocated budget exact boundary'
  );
  destinationNow += 50;
  check(
    destination.shoot('a', new ShootRequest(new Vec3(0, 1, 0), 4)).result !== null,
    'relocated cooldown exact boundary'
  );
  const lethal = positioned(ArenaMap.spawns[0], ArenaMap.spawns[1]);
  for (let sequence = 1; sequence <= 4; sequence++) {
    lethal.shoot('a', new ShootRequest(new Vec3(1, 0, 0), sequence));
    now += Game.fireIntervalMs;
  }
  check(
    lethal.phase === MatchPhase.Finished && lethal.winner === 'a' && lethal.view()[1].hp === 0,
    'four hits finish match'
  );
  before = lethal.capture();
  check(
    lethal.move('a', new MoveRequest(new Vec3(0, 0, 1), 5))?.reason === Rejection.matchFinished &&
      lethal.shoot('a', new ShootRequest(new Vec3(1, 0, 0), 5)).rejected?.reason ===
        Rejection.matchFinished &&
      equal(before, lethal.capture()),
    'finished actions leave state unchanged'
  );
  const finished = new RoomState(() => destinationNow);
  finished.restore(before);
  check(
    finished.phase === MatchPhase.Finished && finished.winner === 'a' &&
      equal(finished.view(), lethal.view()),
    'finished snapshot preserves winner and sequence'
  );
  check(
    room.view().concat(destination.view(), finished.view()).every((player) => player.position.y === 0),
    'feet remain on ground through movement and relocation'
  );
  assert.equal(checkedCount, 45, 'Domain boundary count differs from .NET');
  console.log(`Domain boundaries: ${checkedCount}/${checkedCount} passed`);

  function positioned(a: Vec3, b: Vec3): RoomState {
    const result = new RoomState(() => now);
    result.restore({
      players: [
        { playerId: 'a', position: a, hp: 100, sequence: 0, fireRemainingMs: 0, moveRemainingMs: 0 },
        { playerId: 'b', position: b, hp: 100, sequence: 0, fireRemainingMs: 0, moveRemainingMs: 0 }
      ],
      phase: MatchPhase.Playing,
      winner: null
    });
    return result;
  }
}
