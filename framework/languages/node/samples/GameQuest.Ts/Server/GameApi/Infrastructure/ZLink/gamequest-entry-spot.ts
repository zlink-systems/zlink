import { GameQuestPlayerActor } from './gamequest-player-actor';
import type {
  ZLinkEntrySpot,
  ZLinkEntrySpotContext,
  ZLinkActorCreateResponse,
  ZLinkMessage,
} from '@zlink-systems/framework';

class GameQuestEntrySpot implements ZLinkEntrySpot<GameQuestPlayerActor> {
  readonly context!: ZLinkEntrySpotContext<GameQuestPlayerActor>;

  async onCreateActor(_actor: GameQuestPlayerActor, _request: ZLinkMessage): Promise<ZLinkActorCreateResponse> {
    return { accepted: true };
  }
  async onJoinedActor(_actor: GameQuestPlayerActor): Promise<void> {}
  async onLeaveActor(_actor: GameQuestPlayerActor): Promise<void> {}
  async onDisconnectActor(_actor: GameQuestPlayerActor): Promise<void> {}
}

export { GameQuestEntrySpot };
