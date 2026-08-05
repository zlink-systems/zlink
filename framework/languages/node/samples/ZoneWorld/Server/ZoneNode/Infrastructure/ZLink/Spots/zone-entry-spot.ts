import type {
  ZLinkEntrySpot,
  ZLinkEntrySpotContext,
  ZLinkMessage,
  ZLinkSpotActorJoinResult
} from '@zlink-systems/framework';
import type { PlayerActor } from '../Actors/player-actor';

class ZoneEntrySpot implements ZLinkEntrySpot<PlayerActor> {
  readonly context!: ZLinkEntrySpotContext<PlayerActor, ZoneEntrySpot>;

  async onActorJoin(_actorId: string, _request: ZLinkMessage): Promise<ZLinkSpotActorJoinResult> {
    return { accepted: true };
  }

  async onJoinedActor(_actor: PlayerActor): Promise<void> {}

  async onLeaveActor(_actor: PlayerActor): Promise<void> {}

  async onDisconnectActor(_actor: PlayerActor): Promise<void> {}
}


export { ZoneEntrySpot };
