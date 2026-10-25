import { randomUUID } from 'node:crypto';
import { Names } from './constants';
export class JoinLobbyRequest {
  constructor(readonly playerId: string) {}
}
export class LobbyView {}
export class CreatePartyRequest {}
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
    readonly ready: boolean[] = [],
    readonly phase: RunPhase = RunPhase.Forming,
    readonly runId: string | null = null
  ) {}
}
export class DungeonView {
  constructor(
    readonly dungeonId: string = '',
    readonly nodeId: string = '',
    readonly partyId: string = '',
    readonly members: string[] = [],
    readonly stage: bigint = 0n,
    readonly runId: string | null = null,
    readonly phase: RunPhase = RunPhase.Preparing
  ) {}
}
export class PrepareDungeon {
  constructor(
    readonly runId: string,
    readonly partyId: string,
    readonly members: string[]
  ) {}
}
export class DungeonPrepared {
  constructor(readonly dungeonId: string, readonly runId: string) {}
}
export class JoinDungeon {
  constructor(
    readonly dungeonId: string,
    readonly runId: string
  ) {}
}
export class ActionRejected {
  constructor(readonly reason: string = "", readonly retryAfterMs: number = 0) {}
}
export class SessionInfoRequest {}
export class SessionInfo {
  constructor(
    readonly sessionId: string,
    readonly actorId: string,
    readonly objectGeneration: bigint
  ) {}
}

export const MaximumInteger = 9007199254740991n;
export const FinalStage = 3n;
export enum RunPhase { Forming = 'Forming', Preparing = 'Preparing', Entering = 'Entering', Active = 'Active', Completed = 'Completed' }
export class RunStarted { constructor(readonly runId: string) {} }
export class RunCompleted { constructor(readonly runId: string) {} }
export class EnterPartyRun { constructor(readonly playerId: string) {} }

export class DungeonMemberEntered { constructor(readonly runId: string, readonly playerId: string) {} }

export function newPartyId(): string { return Names.partyPrefix + randomUUID().replaceAll('-', ''); }
export function newRunId(): string { return 'run-' + randomUUID().replaceAll('-', ''); }
