import { MoveRejectReasons, ZoneWorldSpec, zoneOf } from '../../../Shared/spec';
import { inRange, isNorth, isWest } from './world';
import type { MoveRejectReason, ZoneId } from '../../../Shared/spec';
import type { PlayerPosition } from './world';

type MoveDecision =
  | { kind: 'accepted'; to: PlayerPosition; zoneChanged: boolean }
  | { kind: 'rejected'; reason: MoveRejectReason };

function validateMove(
  from: PlayerPosition,
  toX: number,
  toY: number,
  zoneIsUnreachable: (zoneId: ZoneId) => boolean
): MoveDecision {
  if (!inRange(toX, toY)) return { kind: 'rejected', reason: MoveRejectReasons.outOfRange };
  if (
    Math.abs(toX - from.x) > ZoneWorldSpec.maxStepPerAxis
    || Math.abs(toY - from.y) > ZoneWorldSpec.maxStepPerAxis
  ) return { kind: 'rejected', reason: MoveRejectReasons.tooFar };

  const fromZone = zoneOf(from.x, from.y);
  const toZone = zoneOf(toX, toY);
  if (isWest(fromZone) !== isWest(toZone) && isNorth(fromZone) !== isNorth(toZone)) {
    return { kind: 'rejected', reason: MoveRejectReasons.diagonalCrossing };
  }
  const zoneChanged = fromZone !== toZone;
  if (zoneChanged && zoneIsUnreachable(toZone)) {
    return { kind: 'rejected', reason: MoveRejectReasons.zoneMaintenance };
  }
  return { kind: 'accepted', to: { x: toX, y: toY }, zoneChanged };
}

export { validateMove };
export type { MoveDecision };
