import type { ZLinkActorRelocationAdapter } from '@zlink-systems/framework';
import { PlayerActor } from './player-actor';
import type { ZoneId } from '../../../../../Shared/spec';

type PlayerTransferState = {
  x: number;
  y: number;
  zoneId: ZoneId;
  isBot: boolean;
  dirX: number;
  dirY: number;
  pendingJoin: ReturnType<PlayerActor['pendingJoin']>;
  processedJoinOperations: ReturnType<PlayerActor['captureProcessedJoinOperations']>;
};

class PlayerActorRelocationAdapter implements ZLinkActorRelocationAdapter<PlayerActor> {
  // --8<-- [start:doc-zw-actor-capture]
  async capture(actor: PlayerActor): Promise<Uint8Array> {
    return new TextEncoder().encode(
      JSON.stringify({
        x: actor.x,
        y: actor.y,
        zoneId: actor.zoneId,
        isBot: actor.isBot,
        dirX: actor.dirX,
        dirY: actor.dirY,
        pendingJoin: actor.pendingJoin(),
        processedJoinOperations: actor.captureProcessedJoinOperations()
      } satisfies PlayerTransferState)
    );
  }
  // --8<-- [end:doc-zw-actor-capture]

  async restore(actor: PlayerActor, payload: Uint8Array): Promise<void> {
    const state = JSON.parse(new TextDecoder().decode(payload)) as PlayerTransferState;
    actor.x = state.x;
    actor.y = state.y;
    actor.zoneId = state.zoneId;
    actor.isBot = state.isBot;
    actor.dirX = state.dirX;
    actor.dirY = state.dirY;
    actor.restoreProcessedJoinOperations(state.processedJoinOperations);
    actor.restorePendingJoin(state.pendingJoin);
  }
}

export { PlayerActorRelocationAdapter };
