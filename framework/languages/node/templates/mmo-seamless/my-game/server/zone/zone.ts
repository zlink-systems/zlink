import { Inject, Injectable, Scope } from '@nestjs/common';
import { ZLINK_ACTOR_CLIENT } from '@zlink-systems/nestjs';
import type {
  ZLinkActorClient,
  ZLinkEntrySpot,
  ZLinkEntrySpotContext,
  ZLinkMessage,
  ZLinkSpot,
  ZLinkSpotContext,
  ZLinkSpotRelocationAdapter
} from '@zlink-systems/framework';
import { NODE_CONFIG, type NodeConfig } from '../shared/config';
import { Names, Game, adjacentZones, borderTopic } from '../shared/constants';
import { BorderPresence, EnterZone, PlayerAppeared, PlayerLeft, PlayerMoved, type PlayerLeftReason, Position, WorldState } from '../shared/messages';
import { Player } from './player';
import { BorderHandler } from './handlers';
import { ZonePositionDomain, type PlayerView, type ZoneSnapshot } from './domain';
@Injectable({ scope: Scope.TRANSIENT })
export class Entry implements ZLinkEntrySpot<Player> {
  readonly context!: ZLinkEntrySpotContext<Player, Entry>;
  async onJoinedActor(_actor: Player): Promise<void> {}
  async onLeaveActor(_actor: Player): Promise<void> {}
}
@Injectable({ scope: Scope.TRANSIENT })
export class Zone implements ZLinkSpot<Player> {
  readonly context!: ZLinkSpotContext<Player, Zone>;
  private positions!: ZonePositionDomain;
  constructor(
    @Inject(ZLINK_ACTOR_CLIENT) private readonly actors: ZLinkActorClient,
    @Inject(NODE_CONFIG) private readonly config: NodeConfig
  ) {}
  configure(): void {
    this.positions = new ZonePositionDomain(String(this.context.spotId));
    for (const adjacent of adjacentZones(String(this.context.spotId)))
      this.context.handlers.addSubscribe(
        BorderHandler,
        Names.border,
        borderTopic(adjacent, String(this.context.spotId))
      );
  }
  async onInitialize(): Promise<void> {}
  async onActorJoin(actorId: string, request: ZLinkMessage) {
    const arrival = request.decode(EnterZone);
    const rejection = this.positions.queueArrival(actorId, arrival);
    return rejection ? { accepted: false, reply: rejection } : { accepted: true, reply: new WorldState(String(this.context.spotId), this.config.id, arrival.position.x, arrival.sequence) };
  }
  async onJoinedActor(actor: Player): Promise<void> {
    const arrival = this.positions.takeArrival(actor.context.actorId);
    if (!arrival) throw new Error('Zone admission has no movement intent');
    this.positions.addPlayer(actor.context.actorId, arrival);
    await this.publishLocal(actor.context.actorId, new PlayerAppeared(actor.context.actorId, arrival.position));
    await this.publishBorder(actor.context.actorId, arrival.position, arrival.sequence, 'appeared', null);
  }
  async onLeaveActor(actor: Player): Promise<void> {
    void actor;
  }
  async onDisconnectActor(actor: Player): Promise<void> {
    const state = this.positions.state(actor.context.actorId);
    if (state?.connected) {
      this.positions.setConnected(actor.context.actorId, false);
      await this.publishLeft(actor.context.actorId, state, 'disconnected');
    }
  }
  state(actorId: string) {
    return this.positions.state(actorId);
  }
  get nodeId(): string {
    return this.config.id;
  }
  setConnected(actorId: string, connected: boolean) {
    return this.positions.setConnected(actorId, connected);
  }
  move(actorId: string, request: import('../shared/messages').MoveRequest) {
    return this.positions.move(actorId, request);
  }
  async leaveForZone(actorId: string): Promise<void> {
    const state = this.positions.removePlayer(actorId);
    if (state) await this.publishLeft(actorId, state, 'left-zone');
  }
  async publishLocal(playerId: string, event: PlayerAppeared | PlayerMoved | PlayerLeft): Promise<void> {
    for (const [observerId] of this.positions.connectedPlayers())
      if (observerId !== playerId) await this.actors.sendToActor(observerId, event).submit();
  }
  async publishBorder(playerId: string, position: Position, sequence: bigint, kind: 'appeared' | 'moved' | 'left', reason: PlayerLeftReason | null): Promise<void> {
    const id = String(this.context.spotId);
    const offset = position.x % Game.zoneWidth;
    if (offset >= Game.borderRadius && offset < Game.zoneWidth - Game.borderRadius) return;
    for (const target of adjacentZones(id))
      await this.context.outbound.publish(Names.border, borderTopic(id, target), new BorderPresence(id, playerId, position, sequence, kind, reason)).submit();
  }
  async publishLeft(playerId: string, state: PlayerView, reason: PlayerLeftReason): Promise<void> {
    await this.publishLocal(playerId, new PlayerLeft(playerId, reason));
    await this.publishBorder(playerId, state.position, state.sequence, 'left', reason);
  }
  async showBorder(presence: BorderPresence): Promise<void> {
    for (const [observerId, observer] of this.positions.connectedPlayers()) {
      if (observerId === presence.playerId || Math.abs(observer.position.x - presence.position.x) > Game.borderRadius) continue;
      const event = presence.kind === 'appeared' ? new PlayerAppeared(presence.playerId, presence.position) : presence.kind === 'moved' ? new PlayerMoved(presence.playerId, presence.position, presence.sequence) : new PlayerLeft(presence.playerId, presence.reason ?? 'left-zone');
      await this.actors.sendToActor(observerId, event).submit();
    }
  }
  capture(): ZoneSnapshot { return this.positions.capture(); }
  restore(snapshot: ZoneSnapshot): void { this.positions.restore(snapshot); }
}
@Injectable()
export class ZoneRelocation implements ZLinkSpotRelocationAdapter<Zone> {
  async capture(spot: Zone): Promise<Uint8Array> {
    return Buffer.from(JSON.stringify(spot.capture(), (_, value) => typeof value === 'bigint' ? value.toString() : value));
  }
  async restore(spot: Zone, payload: Uint8Array): Promise<void> {
    spot.restore(JSON.parse(Buffer.from(payload).toString('utf8')) as ZoneSnapshot);
  }
}
