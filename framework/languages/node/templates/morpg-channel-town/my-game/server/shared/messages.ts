import { PacketNames } from './constants';
export class SessionInfoRequest {}
export class SessionInfo {
  constructor(
    readonly sessionId: string,
    readonly actorId: string,
    readonly objectGeneration: bigint
  ) {}
}
export class JoinTownRequest {
  constructor(
    readonly playerId: string,
    readonly channel: bigint
  ) {}
}
export class ChangeChannelRequest {
  constructor(readonly channel: bigint) {}
}
export class CreatePartyRequest {}
export class JoinPartyRequest {
  constructor(readonly partyId: string) {}
}
export class EnterDungeonRequest {}
export class AdvanceRequest {}
export class ViewRequest {}
export enum RunPhase {
  Forming = 'Forming',
  Preparing = 'Preparing',
  Entering = 'Entering',
  Active = 'Active',
  Completed = 'Completed'
}
export class PartyView {
  constructor(
    readonly partyId: string,
    readonly members: string[],
    readonly phase: RunPhase = RunPhase.Forming,
    readonly runId: string | null = null
  ) {}
}
export class TownView {
  constructor(
    readonly townId = '',
    readonly nodeId = '',
    readonly players: string[] = [],
    readonly parties: PartyView[] = []
  ) {}
}
export class DungeonView {
  constructor(
    readonly dungeonId = '',
    readonly nodeId = '',
    readonly partyId = '',
    readonly members: string[] = [],
    readonly stage: bigint = 0n,
    readonly runId = '',
    readonly phase: RunPhase = RunPhase.Forming
  ) {}
}
export class PrepareDungeon {
  constructor(
    readonly runId: string,
    readonly partyId: string,
    readonly ownerSpotId: string,
    readonly members: string[]
  ) {}
}
export class DungeonPrepared {
  constructor(
    readonly dungeonId: string,
    readonly runId: string,
    readonly reason: string | null = null
  ) {}
}
export class JoinDungeon {
  constructor(
    readonly dungeonId: string,
    readonly runId: string
  ) {}
}
export class ActionRejected {
  constructor(
    readonly reason: string,
    readonly retryAfterMs = 0
  ) {}
}
export class RunStarted {
  constructor(readonly runId: string) {}
}
export class RunCompleted {
  constructor(readonly runId: string) {}
}
export class PartyViewRequest {
  constructor(readonly playerId: string) {}
}

const pushTypes: ReadonlyMap<
  string,
  () => TownView | DungeonView | ActionRejected
> = new Map<
  string,
  () => TownView | DungeonView | ActionRejected
>([
  [PacketNames.townView, () => new TownView()],
  [PacketNames.dungeonView, () => new DungeonView()],
  [PacketNames.actionRejected, () => new ActionRejected('')]
]);
export function restorePush(
  packetName: string,
  payload: TownView | DungeonView | ActionRejected
): TownView | DungeonView | ActionRejected {
  const create = pushTypes.get(packetName);
  if (!create) throw new Error(`Unknown push ${packetName}`);
  // Restore the declared DTO class before forwarding (#1731).
  return Object.assign(create(), payload);
}
