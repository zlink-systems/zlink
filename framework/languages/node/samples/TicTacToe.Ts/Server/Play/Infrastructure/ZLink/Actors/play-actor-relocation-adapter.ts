import type { ZLinkActorRelocationAdapter } from '@zlink-systems/framework';
import { PlayActor } from './play-actor';

interface PlayActorTransferState {
  readonly displayName: string;
  readonly level: number;
  readonly wins: number;
  readonly roomId?: string;
  readonly pendingJoinRoomId?: string;
  readonly destroyAfterEntrySpotJoin?: boolean;
}

// --8<-- [start:doc-relocation-adapter]
class PlayActorRelocationAdapter implements ZLinkActorRelocationAdapter<PlayActor> {
  async capture(actor: PlayActor): Promise<Uint8Array> {
    return new TextEncoder().encode(JSON.stringify({
      displayName: actor.displayName,
      level: actor.level,
      wins: actor.wins,
      roomId: actor.roomId,
      pendingJoinRoomId: actor.pendingJoinRoomId,
      destroyAfterEntrySpotJoin: actor.destroyAfterEntrySpotJoin
    } satisfies PlayActorTransferState));
  }

  async restore(actor: PlayActor, payload: Uint8Array): Promise<void> {
    const restored = JSON.parse(new TextDecoder().decode(payload)) as PlayActorTransferState;
    actor.displayName = restored.displayName;
    actor.level = restored.level;
    actor.wins = restored.wins;
    actor.roomId = restored.roomId;
    actor.pendingJoinRoomId = restored.pendingJoinRoomId;
    actor.destroyAfterEntrySpotJoin = restored.destroyAfterEntrySpotJoin === true;
  }
}
// --8<-- [end:doc-relocation-adapter]

export { PlayActorRelocationAdapter };
