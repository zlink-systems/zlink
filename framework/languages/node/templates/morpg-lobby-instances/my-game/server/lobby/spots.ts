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
  JoinDungeon
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
        Names.partyPrefix + request.partyId === String(this.context.spotId) &&
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
  async enter(actor: Player): Promise<void> {
    const reason = this.state.entryRejection(actor.context.actorId);
    if (reason) {
      await actor.context.boundSession.send(new ActionRejected(reason)).submit();
      return;
    }
    const dungeonId = Names.dungeonPrefix + this.state.partyId;
    const result = await this.spots
      .getOrCreate(dungeonId, Names.dungeon)
      .inMesh(Names.mesh)
      .submit();
    if (result.state === ZLinkSpotCreateState.Rejected)
      throw new Error('Dungeon creation rejected');
    const members = [...this.state.members];
    const prepared = await this.context.outbound
      .requestToSpot(dungeonId, new PrepareDungeon(this.state.partyId, members))
      .submit<DungeonPrepared>();
    if (prepared.dungeonId !== dungeonId) throw new Error('Dungeon preparation identity mismatch');
    for (const id of members)
      await this.actors.sendToActor(id, new JoinDungeon(dungeonId, this.state.partyId)).submit();
  }
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
