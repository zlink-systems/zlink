import { ZoneIds, ZoneWorldSpec, zoneOf } from '../Shared/spec';
import type { ZoneId } from '../Shared/spec';
import { adjacentZones, isNorth, isWest } from '../Server/ZoneNode/Domain/world';

type Point = { x: number; y: number };

export function boundaryRoute(targetZoneId: string, sourceZoneId: string = ZoneIds.northWest) {
  const source = sourceZoneId as ZoneId;
  const target = targetZoneId as ZoneId;
  if (!adjacentZones(source).includes(target)) {
    throw new Error(`Unsupported Ops-selected boundary '${source}' to '${target}'.`);
  }
  const crossesX = isWest(source) !== isWest(target);
  const split = ZoneWorldSpec.zoneSplit;
  const center = split / 2;
  const edge = split - 1;
  const inside = split + 2;
  const observer = split - ZoneWorldSpec.maxStepPerAxis;
  const transform = (point: Point): Point => ({
    x: isWest(source) ? point.x : ZoneWorldSpec.worldSize - 1 - point.x,
    y: isNorth(source) ? point.y : ZoneWorldSpec.worldSize - 1 - point.y
  });
  const point = (along: number, across: number) =>
    transform(crossesX ? { x: across, y: along } : { x: along, y: across });
  const diagonalBefore = point(edge - 1, center);
  const diagonalInside = point(inside, center);
  return {
    sourceEdge: point(center, edge),
    targetInside: point(center, inside),
    targetContinue: point(center, inside + ZoneWorldSpec.botStep),
    sourceReturn: point(center, edge - 1),
    observer: point(center, observer),
    diagonalZoneId: zoneOf(diagonalInside.x, diagonalInside.y),
    diagonalBefore,
    diagonalInside
  };
}
