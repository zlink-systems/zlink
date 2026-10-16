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
import { EnterWorldRequest, PlayerView } from '../shared/messages';
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
    await this.actors.sendToActor(actor.context.actorId, this.view(actor)).submit();
  }
  async onLeaveActor(_actor: Player): Promise<void> {}
}
// Framework owns Zone identity and membership; there is no application state.
@Injectable()
export class ZoneRelocation implements ZLinkSpotRelocationAdapter<Zone> {
  async capture(_spot: Zone): Promise<Uint8Array> {
    return new Uint8Array();
  }
  async restore(_spot: Zone, _payload: Uint8Array): Promise<void> {}
}
