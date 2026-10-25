import assert from 'node:assert/strict';
import { Game } from '../server/shared/constants';
import { EnterZone, MoveRequest, Position } from '../server/shared/messages';
import { ZonePositionDomain } from '../server/zone/domain';

export function checkDomain(): void {
  let now = 1_000;
  const domain = new ZonePositionDomain('zone-1', () => now);
  domain.addPlayer('player', new EnterZone(new Position(10), 0n, 0));
  let assertions = 0;
  const unchanged = (request: MoveRequest, reason: string): void => {
    const before = domain.state('player')!;
    const result = domain.move('player', request);
    assert.equal(result.rejection?.reason, reason);
    assert.deepEqual(domain.state('player'), before);
    assertions += 2;
  };
  unchanged(new MoveRequest(Number.NaN, 1n), 'non-finite');
  unchanged(new MoveRequest(Game.maxStep + 1, 1n), 'step-too-large');
  unchanged(new MoveRequest(1, BigInt(Game.maxSafeInteger) + 1n), 'integer-out-of-range');
  unchanged(new MoveRequest(1, 0n), 'stale-sequence');
  const edge = new ZonePositionDomain('zone-1', () => now);
  edge.addPlayer('edge', new EnterZone(new Position(0), 0n, 0));
  const edgeBefore = edge.state('edge')!;
  const outside = edge.move('edge', new MoveRequest(-1, 1n));
  assert.equal(outside.rejection?.reason, 'outside-world');
  assert.deepEqual(edge.state('edge'), edgeBefore);
  assertions += 2;
  assert.equal(domain.move('player', new MoveRequest(1, 1n)).accepted, true);
  assertions++;
  const budget = domain.move('player', new MoveRequest(1, 2n));
  assert.equal(budget.rejection?.reason, 'move-budget');
  assert(budget.rejection!.retryAfterMs > 0);
  assertions += 2;
  now += 20;
  assert.equal(domain.move('player', new MoveRequest(1, 2n)).accepted, true);
  assertions++;
  now += 50;
  const snapshot = domain.capture();
  const restored = new ZonePositionDomain('zone-1', () => now);
  restored.restore(snapshot);
  assert.equal(restored.move('player', new MoveRequest(1, 3n)).accepted, true);
  assertions++;
  const arrival = domain.queueArrival(
    'bad',
    new EnterZone(new Position(10), BigInt(Game.maxSafeInteger) + 1n, 0)
  );
  assert.equal(arrival?.reason, 'integer-out-of-range');
  assertions++;
  console.log(`Domain checks: 9/9 passed; assertions: ${assertions}`);
}
