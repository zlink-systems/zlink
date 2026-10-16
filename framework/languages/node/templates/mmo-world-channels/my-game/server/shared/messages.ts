export class EnterWorldRequest {
  constructor(readonly playerId: string) {}
}
export class ChangeChannelRequest {
  constructor(readonly channel: number) {}
}
export class EarnRequest {
  constructor(readonly amount: number) {}
}
export class ViewRequest {}
export class PlayerView {
  constructor(
    readonly worldId = '',
    readonly channel = 0,
    readonly nodeId = '',
    readonly gold = 0
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
export interface WorldInfo {
  worldId: string;
  name: string;
  sessionEndpoint: string;
}
