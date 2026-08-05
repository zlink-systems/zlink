import { ZoneIds, ZoneWorldSpec, zoneOf } from '../../../Shared/spec';
import type { ZoneId } from '../../../Shared/spec';

type PlayerPosition = { x: number; y: number };

function inRange(x: number, y: number): boolean {
  return x >= 0 && x < ZoneWorldSpec.worldSize && y >= 0 && y < ZoneWorldSpec.worldSize;
}

function adjacentZones(zoneId: ZoneId): readonly ZoneId[] {
  switch (zoneId) {
    case ZoneIds.northWest: return [ZoneIds.northEast, ZoneIds.southWest];
    case ZoneIds.northEast: return [ZoneIds.northWest, ZoneIds.southEast];
    case ZoneIds.southWest: return [ZoneIds.northWest, ZoneIds.southEast];
    case ZoneIds.southEast: return [ZoneIds.northEast, ZoneIds.southWest];
  }
}

function isWest(zoneId: ZoneId): boolean {
  return zoneId === ZoneIds.northWest || zoneId === ZoneIds.southWest;
}

function isNorth(zoneId: ZoneId): boolean {
  return zoneId === ZoneIds.northWest || zoneId === ZoneIds.northEast;
}

function inBorderBand(x: number, y: number, fromZoneId: ZoneId, toZoneId: ZoneId): boolean {
  if (zoneOf(x, y) !== fromZoneId) return false;
  const crossesX = isWest(fromZoneId) !== isWest(toZoneId);
  const crossesY = isNorth(fromZoneId) !== isNorth(toZoneId);
  if (crossesX === crossesY) return false;
  const distance = crossesX
    ? Math.abs(isWest(fromZoneId) ? ZoneWorldSpec.zoneSplit - 1 - x : x - ZoneWorldSpec.zoneSplit)
    : Math.abs(isNorth(fromZoneId) ? ZoneWorldSpec.zoneSplit - 1 - y : y - ZoneWorldSpec.zoneSplit);
  return distance < ZoneWorldSpec.borderBand;
}

export { adjacentZones, inBorderBand, inRange, isNorth, isWest };
export type { PlayerPosition };
