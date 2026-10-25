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
  MoveResult,
  ShotResult,
  JoinRoomRequest,
  PrepareRoom,
  RoomPrepared,
  RoomView,
  type MoveRequest,
  type ShootRequest
} from '../shared/messages';
import { RoomState } from './domain';
import { RoomRoster, type HttpRoomSnapshot } from './roster';
import { decodeRoomSnapshot, encodeRoomSnapshot } from './snapshot';
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
  private readonly roster = new RoomRoster();
  constructor(
    @Inject(NODE_CONFIG) private readonly config: NodeConfig,
    @Inject(ZLINK_ACTOR_CLIENT) private readonly actors: ZLinkActorClient
  ) {}
  async onActorJoin(id: string, payload: ZLinkMessage) {
    const admission = payload.decode(JoinRoomRequest);
    if (admission.playerId !== id || admission.roomId !== String(this.context.spotId))
      throw new Error('Room admission identity mismatch');
    return { accepted: this.roster.canAdmit(id) && this.state.admit(id) };
  }
  prepare(request: PrepareRoom): RoomPrepared {
    return new RoomPrepared(this.roster.prepare(String(this.context.spotId), request));
  }
  async onJoinedActor(_actor: Player): Promise<void> {
    await this.broadcast(this.view());
  }
  async onLeaveActor(_actor: Player): Promise<void> {}
  private view(): RoomView {
    return new RoomView(
      String(this.context.spotId),
      this.config.id,
      this.state.Phase,
      this.state.Winner,
      this.state.view()
    );
  }
  async sendView(actor: Player): Promise<void> {
    await actor.context.boundSession.send(this.view()).submit();
  }
  private async broadcast(message: RoomView | MatchEnded): Promise<void> {
    for (const player of this.state.view())
      await this.actors.sendToActor(player.playerId, message).submit();
  }
  async move(actor: Player, request: MoveRequest): Promise<void> {
    const rejection = this.state.move(actor.context.actorId, request);
    if (rejection) await actor.context.boundSession.send(rejection).submit();
    else {
      const player = this.state.view().find((p) => p.playerId === actor.context.actorId)!;
      await actor.context.boundSession.send(new MoveResult(player.x, player.y, request.sequence)).submit();
      await this.broadcast(this.view());
    }
  }
  async shoot(actor: Player, request: ShootRequest): Promise<void> {
    const rejection = this.state.shoot(actor.context.actorId, request);
    if (rejection) {
      await actor.context.boundSession.send(rejection).submit();
      return;
    }
    await actor.context.boundSession.send(new ShotResult('hit', request.targetId, request.sequence)).submit();
    await this.broadcast(this.view());
    if (this.state.Winner !== null) await this.broadcast(new MatchEnded(this.state.Winner));
  }
  capture(): Uint8Array {
    const snapshot: HttpRoomSnapshot = {
      Game: JSON.parse(new TextDecoder().decode(this.state.capture())) as HttpRoomSnapshot['Game'],
      Roster: this.roster.capture()
    };
    return encodeRoomSnapshot(snapshot);
  }
  restore(payload: Uint8Array): void {
    const snapshot = decodeRoomSnapshot<HttpRoomSnapshot>(payload);
    if (!snapshot) throw new Error('Missing room state');
    this.roster.restore(snapshot.Roster);
    this.state.restore(new TextEncoder().encode(JSON.stringify(snapshot.Game)));
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
