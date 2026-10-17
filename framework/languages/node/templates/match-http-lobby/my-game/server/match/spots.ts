import { Inject, Injectable, Scope } from '@nestjs/common';
import { ZLINK_ACTOR_CLIENT } from '@zlink-systems/nestjs';
import {
  type ZLinkActorClient,
  type ZLinkEntrySpot,
  type ZLinkEntrySpotContext,
  type ZLinkMessage,
  type ZLinkSpot,
  type ZLinkSpotContext,
  type ZLinkSpotRelocationAdapter
} from '@zlink-systems/framework';
import { NODE_CONFIG, type NodeConfig } from '../shared/config';
import {
  ActionRejected,
  MatchEnded,
  JoinRoomRequest,
  RoomView,
  ShotRejected,
  type MoveRequest,
  type ShootRequest
} from '../shared/messages';
import { RoomState } from './domain';
import { Player } from './player';
@Injectable({ scope: Scope.TRANSIENT })
export class Entry implements ZLinkEntrySpot<Player> {
  readonly context!: ZLinkEntrySpotContext<Player, Entry>;
  async onJoinedActor(_actor: Player): Promise<void> {}
  async onLeaveActor(_actor: Player): Promise<void> {}
}
@Injectable({ scope: Scope.TRANSIENT })
export class Room implements ZLinkSpot<Player> {
  readonly context!: ZLinkSpotContext<Player, Room>;
  private state = new RoomState();
  constructor(
    @Inject(NODE_CONFIG) private readonly config: NodeConfig,
    @Inject(ZLINK_ACTOR_CLIENT) private readonly actors: ZLinkActorClient
  ) {}
  async onActorJoin(id: string, payload: ZLinkMessage) {
    const admission = payload.decode(JoinRoomRequest);
    if (admission.playerId !== id || admission.roomId !== String(this.context.spotId))
      throw new Error('Room admission identity mismatch');
    return { accepted: this.state.admit(id) };
  }
  async onJoinedActor(_actor: Player): Promise<void> {
    await this.broadcast(this.view());
  }
  async onLeaveActor(_actor: Player): Promise<void> {}
  private view(): RoomView {
    return new RoomView(String(this.context.spotId), this.config.id, this.state.view());
  }
  async sendView(actor: Player): Promise<void> {
    await actor.context.boundSession.send(this.view()).submit();
  }
  private async broadcast(message: RoomView | MatchEnded): Promise<void> {
    for (const player of this.state.players)
      await this.actors.sendToActor(player.playerId, message).submit();
  }
  async move(actor: Player, request: MoveRequest): Promise<void> {
    const reason = this.state.move(actor.context.actorId, request);
    if (reason) await actor.context.boundSession.send(new ActionRejected(reason)).submit();
    else await this.broadcast(this.view());
  }
  async shoot(actor: Player, request: ShootRequest): Promise<void> {
    const reason = this.state.shoot(actor.context.actorId, request);
    if (reason) {
      await actor.context.boundSession.send(new ShotRejected(reason)).submit();
      return;
    }
    await this.broadcast(this.view());
    if (this.state.winner !== null) await this.broadcast(new MatchEnded(this.state.winner));
  }
  capture(): Uint8Array {
    return Buffer.from(JSON.stringify(this.state));
  }
  restore(payload: Uint8Array): void {
    this.state = Object.assign(new RoomState(), JSON.parse(Buffer.from(payload).toString('utf8')));
  }
}
@Injectable()
export class RoomRelocation implements ZLinkSpotRelocationAdapter<Room> {
  async capture(spot: Room): Promise<Uint8Array> {
    return spot.capture();
  }
  async restore(spot: Room, payload: Uint8Array): Promise<void> {
    spot.restore(payload);
  }
}
