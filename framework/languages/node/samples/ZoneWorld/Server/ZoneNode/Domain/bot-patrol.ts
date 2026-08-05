import { BotIds, ZoneIds, ZoneWorldSpec } from '../../../Shared/spec';

type BotRoute = {
  playerId: string;
  x: number;
  y: number;
  dirX: number;
  dirY: number;
  zoneId: string;
};

const botRoutes: readonly BotRoute[] = [
  { playerId: BotIds.northWestX, x: 10, y: 15, dirX: 1, dirY: 0, zoneId: ZoneIds.northWest },
  { playerId: BotIds.northWestY, x: 15, y: 10, dirX: 0, dirY: 1, zoneId: ZoneIds.northWest },
  { playerId: BotIds.northEastX, x: 90, y: 15, dirX: -1, dirY: 0, zoneId: ZoneIds.northEast },
  { playerId: BotIds.northEastY, x: 85, y: 10, dirX: 0, dirY: 1, zoneId: ZoneIds.northEast },
  { playerId: BotIds.southWestX, x: 10, y: 85, dirX: 1, dirY: 0, zoneId: ZoneIds.southWest },
  { playerId: BotIds.southWestY, x: 15, y: 90, dirX: 0, dirY: -1, zoneId: ZoneIds.southWest },
  { playerId: BotIds.southEastX, x: 90, y: 85, dirX: -1, dirY: 0, zoneId: ZoneIds.southEast },
  { playerId: BotIds.southEastY, x: 85, y: 90, dirX: 0, dirY: -1, zoneId: ZoneIds.southEast }
];

function nextBotStep(x: number, y: number, dirX: number, dirY: number): { x: number; y: number } {
  return { x: x + dirX * ZoneWorldSpec.botStep, y: y + dirY * ZoneWorldSpec.botStep };
}

export { botRoutes, nextBotStep };
export type { BotRoute };
