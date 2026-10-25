import { Inject, Injectable, Scope } from '@nestjs/common';
import { ZLINK_ACTOR_CLIENT, ZLINK_SPOT_MANAGER } from '@zlink-systems/nestjs';
import {
  ZLinkSpotCreateState,
  type ZLinkActorClient,
  type ZLinkEntrySpot,
  type ZLinkEntrySpotContext,
  type ZLinkMessage,
  type ZLinkSpot,
  type ZLinkSpotContext,
  type ZLinkSpotManager,
  type ZLinkSpotRelocationAdapter
} from '@zlink-systems/framework';
import { NODE_CONFIG, type NodeConfig } from '../shared/config';
import { Names } from '../shared/constants';
import {
  MatchEnded,
  MatchFound,
  QueueRequest,
  RoomAdmission,
  RoomAssignment,
  RoomView,
  MoveResult,
  type MoveRequest,
  type ShootRequest
} from '../shared/messages';
import { MatchmakingState } from './matchmaking-state';
import { RoomState, type RoomSnapshot } from './domain';
import { Player } from './player';
@Injectable({ scope: Scope.TRANSIENT })
export class Entry implements ZLinkEntrySpot<Player> {
  readonly context!: ZLinkEntrySpotContext<Player, Entry>;
  async onJoinedActor(_actor: Player): Promise<void> {}
  async onLeaveActor(_actor: Player): Promise<void> {}
}
@Injectable({ scope: Scope.TRANSIENT })
export class Matchmaking implements ZLinkSpot<Player> {
  readonly context!: ZLinkSpotContext<Player, Matchmaking>;
  private state = new MatchmakingState();
  constructor(
    @Inject(ZLINK_SPOT_MANAGER) private readonly spots: ZLinkSpotManager,
    @Inject(ZLINK_ACTOR_CLIENT) private readonly actors: ZLinkActorClient
  ) {}
  async onActorJoin(id: string, payload: ZLinkMessage) {
    const request = payload.decode(QueueRequest);
    if (request.playerId !== id) throw new Error('Queue identity mismatch');
    this.state.enqueue(request);
    return { accepted: true };
  }
  async onJoinedActor(_actor: Player): Promise<void> {
    await this.match();
  }
  async onLeaveActor(_actor: Player): Promise<void> {}
  async queue(actor: Player, request: QueueRequest): Promise<void> {
    if (request.playerId !== actor.context.actorId) throw new Error('Queue identity mismatch');
    this.state.enqueue(request);
    await this.match();
  }
  private async match(): Promise<void> {
    const pair = this.state.pair();
    if (!pair.length) return;
    const roomId = this.state.allocateRoom();
    const created = await this.spots.getOrCreate(roomId, Names.room).inMesh(Names.mesh).submit();
    if (created.state === ZLinkSpotCreateState.Rejected) throw new Error('Room creation rejected');
    this.state.remove(pair);
    for (const player of pair)
      await this.actors.sendToActor(player.playerId, new RoomAssignment(roomId)).submit();
  }
  capture(): Uint8Array {
    return Buffer.from(JSON.stringify(this.state));
  }
  restore(payload: Uint8Array): void {
    this.state = Object.assign(
      new MatchmakingState(),
      JSON.parse(Buffer.from(payload).toString('utf8'))
    );
  }
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
    const admission = payload.decode(RoomAdmission);
    if (admission.playerId !== id || admission.roomId !== String(this.context.spotId))
      throw new Error('Room admission identity mismatch');
    return { accepted: this.state.admit(id) };
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
    if (!player) throw new Error('Unknown room player');
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
    if (!decision.result) throw new Error('Accepted shot has no result');
    await actor.context.boundSession.send(decision.result).submit();
    await this.broadcast(this.view());
    if (this.state.winner !== null) await this.broadcast(new MatchEnded(this.state.winner));
  }
  capture(): Uint8Array {
    return Buffer.from(JSON.stringify(this.state.capture()));
  }
  restore(payload: Uint8Array): void {
    this.state = new RoomState();
    this.state.restore(JSON.parse(Buffer.from(payload).toString('utf8')) as RoomSnapshot);
  }
}
@Injectable()
export class MatchmakingRelocation implements ZLinkSpotRelocationAdapter<Matchmaking> {
  async capture(spot: Matchmaking): Promise<Uint8Array> {
    return spot.capture();
  }
  async restore(spot: Matchmaking, payload: Uint8Array): Promise<void> {
    spot.restore(payload);
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
