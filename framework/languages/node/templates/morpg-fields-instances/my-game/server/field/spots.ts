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
import { NODE_CONFIG, type NodeConfig } from '../shared/config';
import {
  Names,
  Game,
  Rejection,
  adjacentFields,
  borderTopic,
  dungeonId
} from '../shared/constants';
import {
  ActionRejected,
  BorderPresence,
  BorderVisible,
  DungeonPrepared,
  EnterField,
  FieldView,
  JoinDungeon,
  PrepareDungeon
} from '../shared/messages';
import { Player } from '../shared/player';
import { FieldState } from './domain';
import { BorderHandler, BorderTick } from './handlers';
@Injectable({ scope: Scope.TRANSIENT })
export class Field implements ZLinkSpot<Player> {
  readonly context!: ZLinkSpotContext<Player, Field>;
  private state = new FieldState();
  constructor(
    @Inject(NODE_CONFIG) private readonly config: NodeConfig,
    @Inject(ZLINK_ACTOR_CLIENT) private readonly actors: ZLinkActorClient,
    @Inject(ZLINK_SPOT_MANAGER) private readonly spots: ZLinkSpotManager
  ) {}
  configure(): void {
    for (const adjacent of adjacentFields(String(this.context.spotId)))
      this.context.handlers.addSubscribe(
        BorderHandler,
        Names.border,
        borderTopic(adjacent, String(this.context.spotId))
      );
  }
  async onInitialize(): Promise<void> {
    await this.context.addTimer(Names.borderTimer, Game.tickMs, BorderTick);
  }
  async onActorJoin(id: string, payload: ZLinkMessage) {
    return {
      accepted: this.state.admit(id, payload.decode(EnterField), String(this.context.spotId))
    };
  }
  async onJoinedActor(actor: Player): Promise<void> {
    actor.apply(this.state.arrive(actor.context.actorId));
  }
  async onLeaveActor(actor: Player): Promise<void> {
    this.state.leave(actor.context.actorId);
  }
  async onDisconnectActor(actor: Player): Promise<void> {
    delete this.state.visible[actor.context.actorId];
  }
  remember(actor: Player): void {
    this.state.visible[actor.context.actorId] = new EnterField(actor.x, actor.sequence);
  }
  async publishBorder(): Promise<void> {
    const source = String(this.context.spotId);
    for (const [id, position] of Object.entries(this.state.visible)) {
      const offset = position.x % Game.fieldWidth;
      if (offset >= Game.borderRadius && offset < Game.fieldWidth - Game.borderRadius) continue;
      for (const adjacent of adjacentFields(source))
        await this.context.outbound
          .publish(
            Names.border,
            borderTopic(source, adjacent),
            new BorderPresence(source, id, position.x, position.sequence)
          )
          .submit();
    }
  }
  async showBorder(presence: BorderPresence): Promise<void> {
    for (const [id, observer] of Object.entries(this.state.visible))
      if (Math.abs(observer.x - presence.x) <= Game.borderRadius)
        await this.actors
          .sendToActor(
            id,
            new BorderVisible(
              String(this.context.spotId),
              presence.sourceFieldId,
              presence.playerId,
              presence.x,
              presence.sequence
            )
          )
          .submit();
  }
  async party(actor: Player, partyId: string, create: boolean): Promise<void> {
    const reason = this.state.party(actor.context.actorId, partyId, create);
    if (reason) {
      await actor.context.boundSession.send(new ActionRejected(reason)).submit();
      return;
    }
    const view = new FieldView(
      String(this.context.spotId),
      this.config.id,
      Object.keys(this.state.visible).sort(),
      this.state.partyViews()
    );
    await actor.context.boundSession.send(view).submit();
    for (const id of Object.keys(this.state.visible))
      if (id !== actor.context.actorId) await this.actors.sendToActor(id, view).submit();
  }
  async enter(actor: Player): Promise<void> {
    const party = this.state.ledBy(actor.context.actorId);
    if (!party) {
      await actor.context.boundSession.send(new ActionRejected(Rejection.notLeader)).submit();
      return;
    }
    const [id, currentMembers] = party,
      target = dungeonId(id),
      members = [...currentMembers];
    const created = await this.spots.getOrCreate(target, Names.dungeon).inMesh(Names.mesh).submit();
    if (created.state === ZLinkSpotCreateState.Rejected)
      throw new Error('Dungeon creation rejected');
    const prepared = await this.context.outbound
      .requestToSpot(target, new PrepareDungeon(id, members))
      .submit<DungeonPrepared>();
    if (prepared.dungeonId !== target) throw new Error('Dungeon preparation identity mismatch');
    for (const member of members)
      await this.actors.sendToActor(member, new JoinDungeon(target, id)).submit();
  }
  capture(): Uint8Array {
    return Buffer.from(JSON.stringify(this.state));
  }
  restore(payload: Uint8Array): void {
    this.state.restore(JSON.parse(Buffer.from(payload).toString('utf8')) as FieldState);
  }
}
@Injectable()
export class FieldRelocation implements ZLinkSpotRelocationAdapter<Field> {
  async capture(spot: Field): Promise<Uint8Array> {
    return spot.capture();
  }
  async restore(spot: Field, payload: Uint8Array): Promise<void> {
    spot.restore(payload);
  }
}
