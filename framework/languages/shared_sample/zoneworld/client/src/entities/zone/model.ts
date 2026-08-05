import {
  BORDER_BAND,
  NODE_EAST,
  NODE_WEST,
  WORLD_SIZE,
  ZONE_NE,
  ZONE_NW,
  ZONE_SE,
  ZONE_SPLIT,
  ZONE_SW,
  type zoneId,
} from '../../shared/config/world';

export function zoneOf(x: number, y: number): zoneId {
  const east = x >= ZONE_SPLIT;
  const south = y >= ZONE_SPLIT;
  if (!east && !south) return ZONE_NW;
  if (east && !south) return ZONE_NE;
  if (!east) return ZONE_SW;
  return ZONE_SE;
}

export function nodeOf(zoneId: string): string {
  return zoneId === ZONE_NE || zoneId === ZONE_SE ? NODE_EAST : NODE_WEST;
}

export function isBorderBand(x: number, y: number): boolean {
  return Math.abs(x - ZONE_SPLIT) <= BORDER_BAND || Math.abs(y - ZONE_SPLIT) <= BORDER_BAND;
}

export function isWorldCoordinate(value: number): boolean {
  return Number.isInteger(value) && value >= 0 && value < WORLD_SIZE;
}

export function isAdjacentZone(origin: string, candidate: string): boolean {
  if (origin === candidate) return false;
  const diagonal = (origin === ZONE_NW && candidate === ZONE_SE)
    || (origin === ZONE_SE && candidate === ZONE_NW)
    || (origin === ZONE_NE && candidate === ZONE_SW)
    || (origin === ZONE_SW && candidate === ZONE_NE);
  return !diagonal;
}
