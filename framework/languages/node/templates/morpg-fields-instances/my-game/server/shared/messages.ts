export class JoinWorldRequest {
  constructor(
    readonly playerId: string = '',
    readonly x: number = 0
  ) {}
}
export class MoveRequest {
  constructor(
    readonly delta: number = 0,
    readonly sequence: bigint = 0n
  ) {}
}
export class EnterField {
  constructor(
    readonly x: number = 0,
    readonly sequence: bigint = 0n,
    readonly remainingMs: number = 0
  ) {}
}
export class WorldState {
  constructor(
    readonly fieldId: string = '',
    readonly nodeId: string = '',
    readonly x: number = 0,
    readonly sequence: bigint = 0n
  ) {}
}
export class BorderPresence {
  constructor(
    readonly sourceFieldId: string = '',
    readonly playerId: string = '',
    readonly x: number = 0,
    readonly sequence: bigint = 0n
  ) {}
}
export class BorderVisible {
  constructor(
    readonly observerFieldId: string = '',
    readonly sourceFieldId: string = '',
    readonly playerId: string = '',
    readonly x: number = 0,
    readonly sequence: bigint = 0n
  ) {}
}
export class CreatePartyRequest {}
export class JoinPartyRequest {
  constructor(readonly partyId: string = '') {}
}
export class EnterDungeonRequest {}
export class AdvanceRequest {}
export class ViewRequest {}
export class PartyView {
  constructor(
    readonly partyId: string = '',
    readonly members: string[] = [],
    readonly leaderId: string = '',
    readonly phase: RunPhase = 'Forming',
    readonly runId: string | null = null
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
    readonly stage: number = 0,
    readonly runId: string = '',
    readonly phase: RunPhase = 'Preparing'
  ) {}
}
export class PrepareDungeon {
  constructor(
    readonly runId: string = '',
    readonly partyId: string = '',
    readonly members: string[] = []
  ) {}
}
export class DungeonPrepared {
  constructor(readonly dungeonId: string = '', readonly runId: string = '',
    readonly rejection: ActionRejected | null = null) {}
}
export class JoinDungeon {
  constructor(
    readonly dungeonId: string = '',
    readonly runId: string = ''
  ) {}
}
export class ActionRejected {
  constructor(readonly reason: string = '', readonly retryAfterMs: number = 0) {}
}
export class SessionInfoRequest {}
export class SessionInfo {
  constructor(
    readonly sessionId: string = '',
    readonly actorId: string = '',
    readonly objectGeneration: bigint = 0n
  ) {}
}

export const MaxSafeInteger = 9_007_199_254_740_991n;
export type RunPhase = 'Forming' | 'Preparing' | 'Entering' | 'Active' | 'Completed';
export class PartyCommand {
  constructor(readonly actorId = '', readonly join = false, readonly enter = false) {}
}
export class PartyCommandResult {
  constructor(readonly view: PartyView | null = null, readonly rejection: ActionRejected | null = null) {}
}
export class DungeonMemberEntered {
  constructor(readonly runId = '', readonly actorId = '') {}
}
export class RunStarted { constructor(readonly runId = '') {} }
export class RunCompleted { constructor(readonly runId = '') {} }
