export class JoinLobbyRequest {
  constructor(readonly playerId: string) {}
}
export class LobbyView {}
export class CreatePartyRequest {
  constructor(readonly partyId: string) {}
}
export class JoinPartyRequest {
  constructor(readonly partyId: string) {}
}
export class PartyJoin {
  constructor(
    readonly partyId: string,
    readonly create: boolean
  ) {}
}
export class ReadyRequest {}
export class EnterDungeonRequest {}
export class AdvanceRequest {}
export class ViewRequest {}
export class PartyView {
  constructor(
    readonly partyId: string = '',
    readonly members: string[] = [],
    readonly ready: boolean[] = []
  ) {}
}
export class DungeonView {
  constructor(
    readonly dungeonId: string = '',
    readonly nodeId: string = '',
    readonly partyId: string = '',
    readonly members: string[] = [],
    readonly stage: number = 0
  ) {}
}
export class PrepareDungeon {
  constructor(
    readonly partyId: string,
    readonly members: string[]
  ) {}
}
export class DungeonPrepared {
  constructor(readonly dungeonId: string) {}
}
export class JoinDungeon {
  constructor(
    readonly dungeonId: string,
    readonly partyId: string
  ) {}
}
export class ActionRejected {
  constructor(readonly reason: string) {}
}
export class SessionInfoRequest {}
export class SessionInfo {
  constructor(
    readonly sessionId: string,
    readonly actorId: string,
    readonly objectGeneration: bigint
  ) {}
}
