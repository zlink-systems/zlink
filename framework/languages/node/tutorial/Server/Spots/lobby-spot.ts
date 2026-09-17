import { Injectable, Scope } from '@nestjs/common';
import type {
  ZLinkActor,
  ZLinkEntrySpot,
  ZLinkEntrySpotContext
} from '@zlink-systems/framework';

// --8<-- [start:entry-spot]
// Every new player lands here before joining a room, and returns here after
// leaving one. A node that hosts players registers exactly one of these.
@Injectable({ scope: Scope.TRANSIENT })
class LobbySpot implements ZLinkEntrySpot {
  readonly context!: ZLinkEntrySpotContext;

  // Runs after the move is committed, on the side the player arrived at.
  async onJoinedActor(_actor: ZLinkActor): Promise<void> {}

  // Runs on the side the player left. The player still exists elsewhere.
  async onLeaveActor(_actor: ZLinkActor): Promise<void> {}
}
// --8<-- [end:entry-spot]

export { LobbySpot };
