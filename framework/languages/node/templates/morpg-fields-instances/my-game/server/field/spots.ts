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
import {
  Names,
  Game,
  adjacentFields,
  borderTopic,
  fieldId
} from '../shared/constants';
import {
  ActionRejected,
  BorderPresence,
  BorderVisible,
  EnterField,
  MoveRequest, PartyCommand, PartyCommandResult, PartyView
} from '../shared/messages';
import { Player } from '../shared/player';
import { FieldState } from './domain';
import { newId, validId } from './party-domain';
import { BorderHandler, BorderTick } from './handlers';
@Injectable({ scope: Scope.TRANSIENT })
export class Field implements ZLinkSpot<Player> {
  readonly context!: ZLinkSpotContext<Player, Field>;
  private state = new FieldState();
  constructor(
    @Inject(ZLINK_ACTOR_CLIENT) private readonly actors: ZLinkActorClient,
    @Inject(ZLINK_SPOT_MANAGER) private readonly spots: ZLinkSpotManager
  ) {}
  configure(): void {
    this.context.handlers.addTimerHandler(BorderTick);
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
    this.state.connected(actor.context.actorId, false);
  }
  remember(actor: Player): void {
    this.state.connected(actor.context.actorId, true);
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
  async move(actor: Player, request: MoveRequest): Promise<void> {
    const result = this.state.move(actor.context.actorId, request);
    if (result instanceof ActionRejected) {
      await actor.context.boundSession.send(result).submit(); return;
    }
    if (fieldId(result.x) !== String(this.context.spotId)) actor.move(result);
    else { actor.apply(result); await actor.sendState(); }
  }
  async party(actor: Player, partyId: string, create: boolean): Promise<void> {
    if (actor.partyId !== null || (!create && (!validId(partyId, 'party-')
      || !(await this.spots.find(partyId))))) {
      await actor.context.boundSession.send(new ActionRejected('invalid-party')).submit(); return;
    }
    if (create) {
      partyId = newId('party-');
      const result = await this.spots.getOrCreate(partyId, Names.party).inMesh(Names.mesh).submit();
      if (result.state === ZLinkSpotCreateState.Rejected) throw new Error('Party creation rejected');
    }
    const response = await this.context.outbound.requestToSpot(partyId,
      new PartyCommand(actor.context.actorId, true)).submit<PartyCommandResult>();
    if (response.rejection) await actor.context.boundSession.send(Object.assign(new ActionRejected(), response.rejection)).submit();
    else actor.partyId = partyId;
  }
  async enter(actor: Player): Promise<void> {
    if (!actor.partyId) {
      await actor.context.boundSession.send(new ActionRejected('not-leader')).submit(); return;
    }
    const result = await this.context.outbound.requestToSpot(actor.partyId,
      new PartyCommand(actor.context.actorId, false, true)).submit<PartyCommandResult>();
    if (result.rejection) await actor.context.boundSession.send(Object.assign(new ActionRejected(), result.rejection)).submit();
  }
  async show(actor: Player): Promise<void> {
    await actor.sendState();
    if (!actor.partyId) return;
    const result = await this.context.outbound.requestToSpot(actor.partyId,
      new PartyCommand(actor.context.actorId)).submit<PartyCommandResult>();
    if (result.view) await actor.context.boundSession.send(Object.assign(new PartyView(), result.view)).submit();
  }
  capture(): Uint8Array {
    return Buffer.from(JSON.stringify(this.state.capture(), (_, value) => typeof value === 'bigint' ? value.toString() : value));
  }
  restore(payload: Uint8Array): void {
    this.state.restore(JSON.parse(Buffer.from(payload).toString('utf8')));
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
