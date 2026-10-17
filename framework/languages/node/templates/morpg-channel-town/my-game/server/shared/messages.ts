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
    readonly channel: number
  ) {}
}
export class ChangeChannelRequest {
  constructor(readonly channel: number) {}
}
export class CreatePartyRequest {
  constructor(readonly partyId: string) {}
}
export class JoinPartyRequest {
  constructor(readonly partyId: string) {}
}
export class EnterDungeonRequest {}
export class AdvanceRequest {}
export class ViewRequest {}
export class PartyView {
  constructor(
    readonly partyId: string,
    readonly members: string[]
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
    readonly stage = 0
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

const pushTypes: ReadonlyMap<string, typeof TownView | typeof DungeonView> = new Map<
  string,
  typeof TownView | typeof DungeonView
>([
  [PacketNames.townView, TownView],
  [PacketNames.dungeonView, DungeonView]
]);
export function restorePush(
  packetName: string,
  payload: TownView | DungeonView
): TownView | DungeonView {
  const type = pushTypes.get(packetName);
  if (!type) throw new Error(`Unknown push ${packetName}`);
  // Restore the declared DTO class before forwarding (#1731).
  return Object.assign(new type(), payload);
}
