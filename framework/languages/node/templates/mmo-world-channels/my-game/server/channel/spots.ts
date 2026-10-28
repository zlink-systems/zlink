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
import { channelOf } from '../shared/constants';
import { ChannelView, EnterWorldRequest, PlayerView } from '../shared/messages';
import { Player } from './player';
@Injectable({ scope: Scope.TRANSIENT })
export class Entry implements ZLinkEntrySpot<Player> {
  readonly context!: ZLinkEntrySpotContext<Player, Entry>;
  async onJoinedActor(_actor: Player): Promise<void> {}
  async onLeaveActor(_actor: Player): Promise<void> {}
}
@Injectable({ scope: Scope.TRANSIENT })
export class Zone implements ZLinkSpot<Player> {
  readonly context!: ZLinkSpotContext<Player, Zone>;
  private readonly playerIds = new Set<string>();
  constructor(
    @Inject(NODE_CONFIG) private readonly config: NodeConfig,
    @Inject(ZLINK_ACTOR_CLIENT) private readonly actors: ZLinkActorClient
  ) {}
  async onActorJoin(id: string, payload: ZLinkMessage) {
    return { accepted: payload.decode(EnterWorldRequest).playerId === id };
  }
  private view(actor: Player): PlayerView {
    return new PlayerView(
      this.config.worldId,
      channelOf(String(this.context.spotId)),
      this.config.id,
      actor.gold.value
    );
  }
  async show(actor: Player): Promise<void> {
    await actor.context.boundSession.send(this.view(actor)).submit();
  }
  async onJoinedActor(actor: Player): Promise<void> {
    this.playerIds.add(actor.context.actorId);
    await this.actors.sendToActor(actor.context.actorId, this.view(actor)).submit();
    await this.publishPlayers();
  }
  async onLeaveActor(actor: Player): Promise<void> {
    if (!this.playerIds.delete(actor.context.actorId)) return;
    await this.publishPlayers();
  }
  async onDisconnectActor(actor: Player): Promise<void> {
    await this.onLeaveActor(actor);
  }
  private async publishPlayers(): Promise<void> {
    const view = new ChannelView(channelOf(String(this.context.spotId)), [...this.playerIds].sort());
    for (const playerId of this.playerIds) {
      await this.actors.sendToActor(playerId, view).submit();
    }
  }
  async showChannelView(actor: Player): Promise<void> {
    await actor.context.boundSession
      .send(new ChannelView(channelOf(String(this.context.spotId)), [...this.playerIds].sort()))
      .submit();
  }
  snapshot(): string[] {
    return [...this.playerIds].sort();
  }
  restore(playerIds: readonly string[]): void {
    this.playerIds.clear();
    for (const playerId of playerIds) this.playerIds.add(playerId);
  }
}
@Injectable()
export class ZoneRelocation implements ZLinkSpotRelocationAdapter<Zone> {
  async capture(spot: Zone): Promise<Uint8Array> {
    return Buffer.from(JSON.stringify(spot.snapshot()));
  }
  async restore(spot: Zone, payload: Uint8Array): Promise<void> {
    spot.restore(JSON.parse(Buffer.from(payload).toString('utf8')) as string[]);
  }
}
