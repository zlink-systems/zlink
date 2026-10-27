import { Inject, Injectable, Scope } from '@nestjs/common';
import { ZLINK_ACTOR_CLIENT, ZLINK_SPOT_MANAGER } from '@zlink-systems/nestjs';
import {
  ZLinkSpotCreateState,
  type ZLinkActorClient,
  type ZLinkMessage,
  type ZLinkSpot,
  type ZLinkSpotContext,
  type ZLinkSpotManager,
  type ZLinkSpotRelocationAdapter
} from '@zlink-systems/framework';
import { Names } from '../shared/constants';
import {
  ActionRejected,
  PartyJoin,
  PrepareDungeon,
  DungeonPrepared,
  JoinDungeon, RunPhase
} from '../shared/messages';
import { Player } from '../shared/player';
import { PartyState } from './domain';
@Injectable({ scope: Scope.TRANSIENT })
export class Party implements ZLinkSpot<Player> {
  readonly context!: ZLinkSpotContext<Player, Party>;
  private state = new PartyState();
  constructor(
    @Inject(ZLINK_SPOT_MANAGER) private readonly spots: ZLinkSpotManager,
    @Inject(ZLINK_ACTOR_CLIENT) private readonly actors: ZLinkActorClient
  ) {}
  async onActorJoin(id: string, payload: ZLinkMessage) {
    const request = payload.decode(PartyJoin);
    return {
      accepted:
        request.partyId === String(this.context.spotId) &&
        this.state.join(id, request)
    };
  }
  async onJoinedActor(_actor: Player): Promise<void> {
    await this.broadcast();
  }
  async onLeaveActor(actor: Player): Promise<void> {
    this.state.leave(actor.context.actorId);
  }
  async show(actor: Player): Promise<void> {
    await actor.context.boundSession.send(this.state.view()).submit();
  }
  private async broadcast(): Promise<void> {
    const view = this.state.view();
    for (const id of this.state.members) await this.actors.sendToActor(id, view).submit();
  }
  async ready(actor: Player): Promise<void> {
    this.state.markReady(actor.context.actorId);
    await this.broadcast();
  }
  async enter(actor: Player): Promise<void> { await this.enterPlayer(actor.context.actorId); }
  async enterPlayer(playerId: string): Promise<void> {
    const reason = this.state.enter(playerId);
    if (reason) {
      await this.actors.sendToActor(playerId, new ActionRejected(reason)).submit();
      return;
    }
    if (this.state.phase === RunPhase.Active) { await this.actors.sendToActor(playerId, this.state.view()).submit(); return; }
    const runId = this.state.runId!;
    const dungeonId = Names.dungeonPrefix + runId;
    const result = await this.spots
      .getOrCreate(dungeonId, Names.dungeon)
      .inMesh(Names.mesh)
      .submit();
    if (result.state === ZLinkSpotCreateState.Rejected)
      throw new Error('Dungeon creation rejected');
    const members = [...this.state.members];
    const prepared = await this.context.outbound
      .requestToSpot(dungeonId, new PrepareDungeon(runId, this.state.partyId, members))
      .submit<DungeonPrepared>();
    if (prepared.dungeonId !== dungeonId || this.state.prepared(prepared.runId)) throw new Error('Dungeon preparation identity mismatch');
    for (const id of members.filter(id => !this.state.entered.includes(id)))
      await this.actors.sendToActor(id, new JoinDungeon(dungeonId, runId)).submit();
  }
  arrived(runId: string, playerId: string): void { const reason = this.state.arrived(runId, playerId); if (reason) throw new Error(reason); }
  started(runId: string): void { const reason = this.state.started(runId); if (reason) throw new Error(reason); }
  completed(runId: string): void { const reason = this.state.completed(runId); if (reason) throw new Error(reason); }
  capture(): Uint8Array {
    return Buffer.from(JSON.stringify(this.state));
  }
  restore(payload: Uint8Array): void {
    this.state = Object.assign(new PartyState(), JSON.parse(Buffer.from(payload).toString('utf8')));
  }
}
@Injectable()
export class PartyRelocation implements ZLinkSpotRelocationAdapter<Party> {
  async capture(spot: Party): Promise<Uint8Array> {
    return spot.capture();
  }
  async restore(spot: Party, payload: Uint8Array): Promise<void> {
    spot.restore(payload);
  }
}
