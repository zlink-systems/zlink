import { Inject, Injectable, Scope } from '@nestjs/common';
import { ZLINK_ACTOR_CLIENT, ZLINK_SPOT_MANAGER } from '@zlink-systems/nestjs';
import {
  ZLinkSpotCreateState,
  type ZLinkActorClient,
  type ZLinkSpotManager,
  type ZLinkEntrySpot,
  type ZLinkEntrySpotContext,
  type ZLinkSpot,
  type ZLinkSpotContext,
  type ZLinkMessage,
  type ZLinkSpotRelocationAdapter
} from '@zlink-systems/framework';
import { NODE_CONFIG, type NodeConfig } from '../shared/config';
import { Game, Names, Rejection, townId, dungeonId } from '../shared/constants';
import {
  JoinTownRequest,
  TownView,
  PartyView,
  ActionRejected,
  PrepareDungeon,
  DungeonPrepared,
  JoinDungeon
} from '../shared/messages';
import { Player } from '../shared/player';
@Injectable({ scope: Scope.TRANSIENT })
export class Entry implements ZLinkEntrySpot<Player> {
  readonly context!: ZLinkEntrySpotContext<Player, Entry>;
  async onJoinedActor(_actor: Player): Promise<void> {}
  async onLeaveActor(_actor: Player): Promise<void> {}
}
interface TownState {
  players: string[];
  parties: [string, string[]][];
}
@Injectable({ scope: Scope.TRANSIENT })
export class Town implements ZLinkSpot<Player> {
  readonly context!: ZLinkSpotContext<Player, Town>;
  private readonly players = new Set<string>();
  private readonly parties = new Map<string, string[]>();
  constructor(
    @Inject(NODE_CONFIG) private readonly config: NodeConfig,
    @Inject(ZLINK_ACTOR_CLIENT) private readonly actors: ZLinkActorClient,
    @Inject(ZLINK_SPOT_MANAGER) private readonly spots: ZLinkSpotManager
  ) {}
  async onActorJoin(actorId: string, request: ZLinkMessage) {
    const join = request.decode(JoinTownRequest);
    if (join.playerId !== actorId || townId(join.channel) !== String(this.context.spotId))
      return { accepted: false };
    this.players.add(actorId);
    return { accepted: true };
  }
  async onJoinedActor(_actor: Player): Promise<void> {
    await this.broadcast();
  }
  async onLeaveActor(actor: Player): Promise<void> {
    const id = actor.context.actorId;
    this.players.delete(id);
    for (const [party, members] of this.parties) {
      const remaining = members.filter((member) => member !== id);
      if (remaining.length) this.parties.set(party, remaining);
      else this.parties.delete(party);
    }
    await this.broadcast();
  }
  // Disconnection preserves channel and party until an explicit move.
  async onDisconnectActor(_actor: Player): Promise<void> {}
  private view(): TownView {
    return new TownView(
      String(this.context.spotId),
      this.config.id,
      [...this.players].sort(),
      [...this.parties]
        .sort(([a], [b]) => a.localeCompare(b))
        .map(([id, members]) => new PartyView(id, [...members]))
    );
  }
  async show(actor: Player): Promise<void> {
    await actor.context.boundSession.send(this.view()).submit();
  }
  private async broadcast(): Promise<void> {
    const view = this.view();
    for (const id of this.players) await this.actors.sendToActor(id, view).submit();
  }
  async party(actor: Player, partyId: string, create: boolean): Promise<void> {
    const id = actor.context.actorId,
      members = this.parties.get(partyId);
    if (
      !partyId.trim() ||
      !this.players.has(id) ||
      [...this.parties.values()].some((p) => p.includes(id)) ||
      (create ? !!members : !members || members.length >= Game.maximumPartyMembers)
    ) {
      await actor.context.boundSession.send(new ActionRejected(Rejection.invalidParty)).submit();
      return;
    }
    if (create) this.parties.set(partyId, [id]);
    else members!.push(id);
    await this.broadcast();
  }
  async enter(actor: Player): Promise<void> {
    const party = [...this.parties].find(([, members]) => members[0] === actor.context.actorId);
    if (!party) {
      await actor.context.boundSession.send(new ActionRejected(Rejection.notLeader)).submit();
      return;
    }
    const [partyId, members] = party,
      target = dungeonId(partyId);
    const created = await this.spots.getOrCreate(target, Names.dungeon).inMesh(Names.mesh).submit();
    if (created.state === ZLinkSpotCreateState.Rejected)
      throw new Error('Dungeon creation rejected');
    const prepared = await this.context.outbound
      .requestToSpot(target, new PrepareDungeon(partyId, [...members]))
      .submit<DungeonPrepared>();
    if (prepared.dungeonId !== target) throw new Error('Dungeon preparation identity mismatch');
    for (const id of [...members])
      await this.actors.sendToActor(id, new JoinDungeon(target, partyId)).submit();
  }
  capture(): TownState {
    return { players: [...this.players], parties: [...this.parties] };
  }
  restore(state: TownState): void {
    this.players.clear();
    this.parties.clear();
    for (const id of state.players) this.players.add(id);
    for (const [id, members] of state.parties) this.parties.set(id, members);
  }
}
@Injectable()
export class TownRelocation implements ZLinkSpotRelocationAdapter<Town> {
  async capture(spot: Town): Promise<Uint8Array> {
    return Buffer.from(JSON.stringify(spot.capture()));
  }
  async restore(spot: Town, payload: Uint8Array): Promise<void> {
    spot.restore(JSON.parse(Buffer.from(payload).toString('utf8')) as TownState);
  }
}
