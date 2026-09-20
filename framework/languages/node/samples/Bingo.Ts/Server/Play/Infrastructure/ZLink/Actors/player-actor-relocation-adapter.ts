import type { ZLinkActorRelocationAdapter } from '@zlink-systems/framework';
import { PlayerActor } from './player-actor';
import { PlayerActorTransferState } from '../../../../../Shared/Contracts/bingo-messages.generated';

class PlayerActorRelocationAdapter implements ZLinkActorRelocationAdapter<PlayerActor> {
  // --8<-- [start:doc-bingo-relocation-adapter]
  async capture(actor: PlayerActor): Promise<Uint8Array> {
    return new TextEncoder().encode(
      JSON.stringify(
        new PlayerActorTransferState({
          displayName: actor.displayName,
          destroyAfterEntrySpotJoin: actor.destroyAfterEntrySpotJoin,
          disconnected: false
        })
      )
    );
  }
  // --8<-- [end:doc-bingo-relocation-adapter]

  async restore(actor: PlayerActor, payload: Uint8Array): Promise<void> {
    const restored = JSON.parse(new TextDecoder().decode(payload)) as PlayerActorTransferState;
    actor.displayName = restored.displayName;
    actor.destroyAfterEntrySpotJoin = restored.destroyAfterEntrySpotJoin;
  }
}

export { PlayerActorRelocationAdapter };
