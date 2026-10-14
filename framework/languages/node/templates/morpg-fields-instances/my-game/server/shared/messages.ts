export class JoinWorldRequest {
  constructor(
    readonly playerId: string = '',
    readonly x: number = 0
  ) {}
}
export class MoveRequest {
  constructor(
    readonly x: number = 0,
    readonly sequence: number = 0
  ) {}
}
export class EnterField {
  constructor(
    readonly x: number = 0,
    readonly sequence: number = 0
  ) {}
}
export class WorldState {
  constructor(
    readonly fieldId: string = '',
    readonly nodeId: string = '',
    readonly x: number = 0,
    readonly sequence: number = 0
  ) {}
}
export class BorderPresence {
  constructor(
    readonly sourceFieldId: string = '',
    readonly playerId: string = '',
    readonly x: number = 0,
    readonly sequence: number = 0
  ) {}
}
export class BorderVisible {
  constructor(
    readonly observerFieldId: string = '',
    readonly sourceFieldId: string = '',
    readonly playerId: string = '',
    readonly x: number = 0,
    readonly sequence: number = 0
  ) {}
}
export class CreatePartyRequest {
  constructor(readonly partyId: string = '') {}
}
export class JoinPartyRequest {
  constructor(readonly partyId: string = '') {}
}
export class EnterDungeonRequest {}
export class AdvanceRequest {}
export class ViewRequest {}
export class PartyView {
  constructor(
    readonly partyId: string = '',
    readonly members: string[] = []
  ) {}
}
export class FieldView {
  constructor(
    readonly fieldId: string = '',
    readonly nodeId: string = '',
    readonly players: string[] = [],
    readonly parties: PartyView[] = []
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
    readonly partyId: string = '',
    readonly members: string[] = []
  ) {}
}
export class DungeonPrepared {
  constructor(readonly dungeonId: string = '') {}
}
export class JoinDungeon {
  constructor(
    readonly dungeonId: string = '',
    readonly partyId: string = ''
  ) {}
}
export class ActionRejected {
  constructor(readonly reason: string = '') {}
}
export class SessionInfoRequest {}
export class SessionInfo {
  constructor(
    readonly sessionId: string = '',
    readonly actorId: string = '',
    readonly objectGeneration: bigint = 0n
  ) {}
}
