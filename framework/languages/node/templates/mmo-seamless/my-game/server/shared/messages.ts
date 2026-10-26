import { PacketNames } from './constants';
export class SessionInfoRequest {}
export class SessionInfo {
  constructor(
    readonly sessionId: string,
    readonly actorId: string,
    readonly objectGeneration: bigint
  ) {}
}
export class JoinWorldRequest {
  constructor(
    readonly playerId: string,
    readonly x: number
  ) {}
}
export class MoveRequest {
  constructor(
    readonly delta: number,
    readonly sequence: bigint
  ) {}
}
export class Position {
  constructor(readonly x: number) {}
}
export class EnterZone {
  constructor(
    readonly position: Position,
    readonly sequence: bigint,
    readonly remainingMs: number
  ) {}
}
export class WorldState {
  constructor(
    readonly zoneId: string,
    readonly nodeId: string,
    readonly x: number,
    readonly sequence: bigint
  ) {}
}
export class ActionRejected {
  constructor(
    readonly reason: string,
    readonly retryAfterMs: number
  ) {}
}
export type PlayerLeftReason = 'left-zone' | 'disconnected' | 'out-of-range';
export type BorderPresenceKind = 'appeared' | 'moved' | 'left';
export class PlayerAppeared {
  constructor(
    readonly playerId: string,
    readonly position: Position,
    readonly sourceSpotId: string = ''
  ) {}
}
export class PlayerMoved {
  constructor(
    readonly playerId: string,
    readonly position: Position,
    readonly sequence: bigint,
    readonly sourceSpotId: string = ''
  ) {}
}
export class PlayerLeft {
  constructor(
    readonly playerId: string,
    readonly reason: PlayerLeftReason,
    readonly sourceSpotId: string = ''
  ) {}
}
export class BorderPresence {
  constructor(
    readonly sourceZoneId: string,
    readonly playerId: string,
    readonly position: Position,
    readonly sequence: bigint,
    readonly kind: BorderPresenceKind,
    readonly reason: PlayerLeftReason | null
  ) {}
}
export class OpenDungeonRequest {
  constructor(readonly dungeonId: string) {}
}
export class DungeonOpened {
  constructor(
    readonly dungeonId: string,
    readonly nodeId: string
  ) {}
}

const notificationTypes: ReadonlyMap<string, () => Notification> = new Map<
  string,
  () => Notification
>([
  [PacketNames.playerAppeared, () => new PlayerAppeared('', new Position(0))],
  [PacketNames.playerMoved, () => new PlayerMoved('', new Position(0), 0n)],
  [PacketNames.playerLeft, () => new PlayerLeft('', 'left-zone')]
]);
export type Notification = PlayerAppeared | PlayerMoved | PlayerLeft;
export function restoreNotification(packetName: string, payload: Notification): Notification {
  const create = notificationTypes.get(packetName);
  if (!create) throw new Error(`Unknown notification ${packetName}`);
  return Object.assign(create(), payload);
}
