import type { ZLinkActorRelocationAdapter } from '@zlink-systems/framework';
import { PlayerActor } from './player-actor';
import { PlayerActorTransferState } from '../../../../../Shared/Contracts/bingo-messages.generated';

class PlayerActorRelocationAdapter implements ZLinkActorRelocationAdapter<PlayerActor> {
  async capture(actor: PlayerActor): Promise<Uint8Array> {
    return new TextEncoder().encode(JSON.stringify(new PlayerActorTransferState({
      displayName: actor.displayName,
      destroyAfterEntrySpotJoin: false,
      disconnected: false
    })));
  }

  async restore(actor: PlayerActor, payload: Uint8Array): Promise<void> {
    const restored = JSON.parse(new TextDecoder().decode(payload)) as PlayerActorTransferState;
    actor.displayName = restored.displayName;
  }
}

export { PlayerActorRelocationAdapter };
