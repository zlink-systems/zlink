import { Inject, Injectable, Scope } from '@nestjs/common';
import { ZLINK_ACTOR_CLIENT, ZLINK_SPOT_MANAGER } from '@zlink-systems/nestjs';
import {
  ZLinkSpotCreateState,
  type ZLinkActorClient,
  type ZLinkSpotManager,
  type ZLinkSpot,
  type ZLinkSpotContext,
  type ZLinkMessage,
  type ZLinkSpotRelocationAdapter
} from '@zlink-systems/framework';
import { NODE_CONFIG, type NodeConfig } from '../shared/config';
import { Game, Names, Rejection, townId, dungeonId } from '../shared/constants';
import { ChannelRules } from '../shared/channel-rules';
import { PartyRun, type PartyRunSnapshot } from './party-run';
import {
  JoinTownRequest,
  TownView,
  ActionRejected,
  PrepareDungeon,
  DungeonPrepared,
  JoinDungeon,
  PartyViewRequest,
  RunStarted,
  RunCompleted,
  RunPhase
} from '../shared/messages';
import { Player } from '../shared/player';
interface TownState {
  players: string[];
  parties: [string, PartyRunSnapshot][];
}
@Injectable({ scope: Scope.TRANSIENT })
export class Town implements ZLinkSpot<Player> {
  readonly context!: ZLinkSpotContext<Player, Town>;
  private readonly players = new Set<string>();
  private readonly parties = new Map<string, PartyRun>();
  constructor(
    @Inject(NODE_CONFIG) private readonly config: NodeConfig,
    @Inject(ZLINK_ACTOR_CLIENT) private readonly actors: ZLinkActorClient,
    @Inject(ZLINK_SPOT_MANAGER) private readonly spots: ZLinkSpotManager
  ) {}
  async onActorJoin(actorId: string, request: ZLinkMessage) {
    const join = request.decode(JoinTownRequest);
    if (
      join.playerId !== actorId ||
      ChannelRules.validate(join.channel) !== null ||
      townId(join.channel) !== String(this.context.spotId)
    )
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
    for (const [partyId, party] of this.parties) {
      party.leave(id);
      if (!party.members.length) this.parties.delete(partyId);
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
        .map(([, party]) => party.view())
    );
  }
  async show(actor: Player): Promise<void> {
    await actor.context.boundSession.send(this.view()).submit();
  }
  private async broadcast(): Promise<void> {
    const view = this.view();
    for (const id of this.players) await this.actors.sendToActor(id, view).submit();
  }
  async party(actor: Player, partyId: string | null, create: boolean): Promise<void> {
    const id = actor.context.actorId;
    const party = partyId === null ? undefined : this.parties.get(partyId);
    if (
      !this.players.has(id) ||
      [...this.parties.values()].some((item) => item.members.includes(id)) ||
      (!create &&
        (!partyId?.trim() ||
          !party ||
          party.phase !== RunPhase.Forming ||
          party.members.length >= Game.maximumPartyMembers))
    ) {
      await actor.context.boundSession.send(new ActionRejected(Rejection.invalidParty)).submit();
      return;
    }
    if (create) {
      const created = new PartyRun(undefined, [id]);
      this.parties.set(created.partyId, created);
    } else party!.members.push(id);
    await this.broadcast();
  }
  async enter(actor: Player): Promise<void> {
    const party = [...this.parties.values()].find((item) =>
      item.members.includes(actor.context.actorId)
    );
    if (!party) {
      await actor.context.boundSession.send(new ActionRejected(Rejection.notLeader)).submit();
      return;
    }
    const reason = party.enter(actor.context.actorId);
    if (reason !== null) {
      await actor.context.boundSession.send(new ActionRejected(reason)).submit();
      return;
    }
    if (party.phase === RunPhase.Active) {
      await this.show(actor);
      return;
    }
    const runId = party.runId!;
    const target = dungeonId(runId);
    const created = await this.spots.getOrCreate(target, Names.dungeon).inMesh(Names.mesh).submit();
    if (created.state === ZLinkSpotCreateState.Rejected)
      throw new Error('Dungeon creation rejected');
    const prepared = await this.context.outbound
      .requestToSpot(
        target,
        new PrepareDungeon(runId, party.partyId, String(this.context.spotId), [...party.members])
      )
      .submit<DungeonPrepared>();
    if (
      prepared.reason !== null ||
      prepared.dungeonId !== target ||
      party.prepared(prepared.runId) !== null
    ) {
      await actor.context.boundSession.send(new ActionRejected(Rejection.runMismatch)).submit();
      return;
    }
    for (const id of party.members)
      if (this.players.has(id))
        await this.actors.sendToActor(id, new JoinDungeon(target, runId)).submit();
  }
  async changeChannel(actor: Player, channel: bigint): Promise<void> {
    const reason =
      ChannelRules.validate(channel) ??
      [...this.parties.values()]
        .find((party) => party.members.includes(actor.context.actorId))
        ?.changeChannel();
    if (reason !== null && reason !== undefined) {
      await actor.context.boundSession.send(new ActionRejected(reason)).submit();
      return;
    }
    actor.context
      .joinSpot(
        townId(channel),
        new JoinTownRequest(actor.context.actorId, channel)
      )
      .defer();
  }
  async runStarted(message: RunStarted): Promise<void> {
    const party = [...this.parties.values()].find((item) => item.runId === message.runId);
    if (!party || party.started(message.runId) !== null) {
      await this.rejectRunMismatch(party);
      return;
    }
    await this.broadcast();
  }
  async runCompleted(message: RunCompleted): Promise<void> {
    const party = [...this.parties.values()].find((item) => item.runId === message.runId);
    if (!party || party.completed(message.runId) !== null) {
      await this.rejectRunMismatch(party);
      return;
    }
    await this.broadcast();
  }
  async showParty(request: PartyViewRequest): Promise<void> {
    await this.actors.sendToActor(request.playerId, this.view()).submit();
  }
  private async rejectRunMismatch(party?: PartyRun): Promise<void> {
    const leader = party?.members[0];
    if (leader) await this.actors.sendToActor(leader, new ActionRejected(Rejection.runMismatch)).submit();
  }
  capture(): TownState {
    return {
      players: [...this.players],
      parties: [...this.parties].map(([partyId, party]) => [partyId, party.capture()])
    };
  }
  restore(state: TownState): void {
    this.players.clear();
    this.parties.clear();
    for (const id of state.players) this.players.add(id);
    for (const [id, party] of state.parties) this.parties.set(id, PartyRun.restore(party));
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
