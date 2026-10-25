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
