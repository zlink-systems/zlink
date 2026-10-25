import { serialize, deserialize } from 'node:v8';
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
  MatchEnded,
  MatchFound,
  MoveResult,
  RoomView,
  type MoveRequest,
  type ShootRequest
} from '../shared/messages';
import { JoinRoomRequest, type PrepareRoom, type RoomPrepared } from '../shared/lobby-messages';
import { RoomState } from './domain';
import { Player } from './player';
import { RoomRoster, type HttpRoomSnapshot } from './roster';
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
  private roster = new RoomRoster();
  constructor(
    @Inject(NODE_CONFIG) private readonly config: NodeConfig,
    @Inject(ZLINK_ACTOR_CLIENT) private readonly actors: ZLinkActorClient
  ) {}
  prepare(request: PrepareRoom): RoomPrepared {
    return this.roster.prepare(String(this.context.spotId), request);
  }
  async onActorJoin(id: string, payload: ZLinkMessage) {
    const admission = payload.decode(JoinRoomRequest);
    if (admission.playerId !== id || admission.roomId !== String(this.context.spotId))
      throw new Error('Room admission identity mismatch');
    return { accepted: this.roster.canAdmit(id) && this.state.admit(id) };
  }
  async onJoinedActor(actor: Player): Promise<void> {
    await this.actors
      .sendToActor(actor.context.actorId, new MatchFound(String(this.context.spotId)))
      .submit();
    await this.broadcast(this.view());
  }
  async onLeaveActor(_actor: Player): Promise<void> {}
  private view(): RoomView {
    return new RoomView(
      String(this.context.spotId),
      this.config.id,
      this.state.view(),
      this.state.phase,
      this.state.winner
    );
  }
  async sendView(actor: Player): Promise<void> {
    await actor.context.boundSession.send(this.view()).submit();
  }
  private async broadcast(message: RoomView | MatchEnded): Promise<void> {
    for (const player of this.state.players)
      await this.actors.sendToActor(player.playerId, message).submit();
  }
  async move(actor: Player, request: MoveRequest): Promise<void> {
    const rejected = this.state.move(actor.context.actorId, request);
    if (rejected) {
      await actor.context.boundSession.send(rejected).submit();
      return;
    }
    const player = this.state.players.find((candidate) => candidate.playerId === actor.context.actorId);
    if (!player) throw new Error('Accepted movement has no player state.');
    await actor.context.boundSession
      .send(new MoveResult(player.position, request.sequence))
      .submit();
    await this.broadcast(this.view());
  }
  async shoot(actor: Player, request: ShootRequest): Promise<void> {
    const decision = this.state.shoot(actor.context.actorId, request);
    if (decision.rejected) {
      await actor.context.boundSession.send(decision.rejected).submit();
      return;
    }
    if (!decision.result) throw new Error('Accepted shot has no result.');
    await actor.context.boundSession.send(decision.result).submit();
    await this.broadcast(this.view());
    if (this.state.winner !== null) await this.broadcast(new MatchEnded(this.state.winner));
  }
  capture(): Uint8Array {
    const snapshot: HttpRoomSnapshot = { Game: this.state.capture(), Roster: this.roster.capture() };
    return serialize(snapshot);
  }
  restore(payload: Uint8Array): void {
    const snapshot = deserialize(Buffer.from(payload)) as HttpRoomSnapshot;
    if (!snapshot?.Game || !snapshot.Roster) throw new Error('Missing Room state.');
    this.roster.restore(snapshot.Roster);
    this.state.restore(snapshot.Game);
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
