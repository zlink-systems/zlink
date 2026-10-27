import assert from 'node:assert/strict';
import { isDeepStrictEqual } from 'node:util';
import { serialize, deserialize } from 'node:v8';
import { Game, Rejection } from '../server/shared/constants';
import { ActionRejected, MoveRequest, PlayerPosition } from '../server/shared/messages';
import { Positions, type PositionSnapshot } from '../server/region/domain/positions';

export function runDomainChecks(): void {
  let count = 0;
  const check = (ok: boolean, name: string): void => { assert(ok, name); count++; };
  const rejected = (p: Positions, id: string, delta: number, sequence: bigint, reason: string, retry: number): boolean => {
    const before = p.capture();
    return isDeepStrictEqual(p.move(id, new MoveRequest(delta, sequence)), new ActionRejected(reason, retry)) && isDeepStrictEqual(before, p.capture());
  };
  check(Game.maxSpeed === Game.maxStep * 5 && Game.maxSafeInteger === 9_007_199_254_740_991n && Game.maxReplicasPerRegion === 2 && Game.allowedRegions.includes('forest') && Game.allowedRegions.includes('desert'), 'shared movement and region limits');
  let now = 0;
  const positions = new Positions(() => now);
  positions.add('p1');
  check(positions.move('p1', new MoveRequest(Game.maxStep, 1n)) === null && isDeepStrictEqual(positions.view(), [new PlayerPosition('p1', Game.maxStep, 1n)]), 'first movement accepts MaxStep');
  check(rejected(positions, 'p1', Game.maxStep, 2n, Rejection.moveBudget, 200), 'budget rejection preserves state');
  const before = positions.capture();
  const fractional = positions.move('p1', new MoveRequest(0.01, 2n));
  check(fractional?.reason === Rejection.moveBudget && fractional.retryAfterMs > 0 && isDeepStrictEqual(before, positions.capture()), 'fractional budget retry');
  check(rejected(positions, 'p1', Game.maxStep + 1, 2n, Rejection.stepTooLarge, 0), 'step maximum plus one');
  check(rejected(positions, 'p1', 1, 1n, Rejection.staleSequence, 0), 'current sequence stale');
  for (const value of [Number.NaN, Number.POSITIVE_INFINITY, Number.NEGATIVE_INFINITY])
    check(rejected(positions, 'p1', value, 2n, Rejection.nonFinite, 0), 'non-finite domain input');
  const edge = new Positions(() => now);
  edge.restore([{ playerId: 'edge', x: Game.regionMaxX, sequence: 0n, remainingMs: 0 }]);
  check(rejected(edge, 'edge', 1, 1n, Rejection.outsideRegion, 0), 'maximum plus one');
  edge.restore([{ playerId: 'edge', x: Game.regionMinX, sequence: 0n, remainingMs: 0 }]);
  check(rejected(edge, 'edge', -1, 1n, Rejection.outsideRegion, 0), 'minimum minus one');
  const safe = new Positions(() => now);
  safe.add('safe');
  check(safe.move('safe', new MoveRequest(0, Game.maxSafeInteger)) === null, 'maximum sequence');
  check(rejected(safe, 'safe', 0, Game.maxSafeInteger + 1n, Rejection.integerOutOfRange, 0), 'maximum sequence plus one');
  const oversized = new Positions(() => now);
  oversized.add('large');
  check(rejected(oversized, 'large', Number.MAX_VALUE, 1n, Rejection.stepTooLarge, 0), 'finite oversized delta');
  const source = new Positions(() => 0);
  source.add('123');
  check(source.move('123', new MoveRequest(Game.maxStep, 1n)) === null, 'snapshot source move');
  const snapshot = source.capture();
  check(snapshot[0]?.remainingMs === 200, 'remaining milliseconds');
  check(isDeepStrictEqual(snapshot, source.capture()), 'capture preserves source');
  let targetNow = 8 * 3600 * 1000;
  const target = new Positions(() => targetNow);
  target.restore(deserialize(serialize(snapshot)) as PositionSnapshot[]);
  check(isDeepStrictEqual(source.view(), target.view()) && target.capture()[0]?.remainingMs === 200, 'different clock origin restore preserves numeric player ID');
  check(rejected(target, '123', Game.maxStep, 2n, Rejection.moveBudget, 200), 'restored budget retained');
  targetNow += 200;
  check(target.move('123', new MoveRequest(Game.maxStep, 2n)) === null, 'server retry boundary');
  console.log(`Domain boundaries: ${count}/20 passed`);
}
