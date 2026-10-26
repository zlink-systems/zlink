import { PacketNames } from './constants';
export class EnterWorldRequest {
  constructor(readonly playerId: string) {}
}
export class ChangeChannelRequest {
  constructor(readonly channel: bigint) {}
}
export class EarnRequest {
  constructor() {}
}
export class ChannelViewRequest {}
export class ChannelView {
  constructor(readonly channelId: bigint, readonly playerIds: string[]) {}
}
export class ActionRejected {
  constructor(readonly reason: string, readonly retryAfterMs: number) {}
}
export class ViewRequest {}
export class PlayerView {
  constructor(
    readonly worldId = '',
    readonly channel = 0n,
    readonly nodeId = '',
    readonly gold = 0n
  ) {}
}
export class SessionInfoRequest {}
export class SessionInfo {
  constructor(
    readonly sessionId: string,
    readonly actorId: string,
    readonly objectGeneration: bigint
  ) {}
}
export const GameRules = {
  maxInteger: 9_007_199_254_740_991n,
  earnAmount: 10n,
  earnIntervalMs: 1_000
} as const;
export const Rejection = {
  earnTooSoon: 'earn-too-soon',
  goldLimit: 'gold-limit',
  integerOutOfRange: 'integer-out-of-range',
  packetNotAllowed: 'packet-not-allowed'
} as const;
export interface WorldInfo {
  worldId: string;
  name: string;
  sessionEndpoint: string;
}

const notificationTypes: ReadonlyMap<string, () => Notification> = new Map<
  string,
  () => Notification
>([
  [PacketNames.playerView, () => new PlayerView()],
  [PacketNames.channelView, () => new ChannelView(0n, [])]
]);
export type Notification = PlayerView | ChannelView;
export function restoreNotification(packetName: string, payload: Notification): Notification {
  const create = notificationTypes.get(packetName);
  if (!create) throw new Error(`Unknown notification ${packetName}`);
  return Object.assign(create(), payload);
}
