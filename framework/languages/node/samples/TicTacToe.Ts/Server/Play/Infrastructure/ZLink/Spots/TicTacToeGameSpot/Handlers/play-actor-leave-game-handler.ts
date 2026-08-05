import { zlinkSpotActorSendHandler } from '@zlink-systems/nestjs';
import type {
  ZLinkMessageContext,
  ZLinkSpotActorSendHandler
} from '@zlink-systems/framework';
import { PlayActor } from '../../../Actors/play-actor';
import type { LeaveGameMsg } from '../../../../../../../Shared/Contracts/messages';
import { PendingActorDestroyRegistry } from '../../EntrySpot/entry-spot-registries';
import { TicTacToeGameSpot } from '../tictactoe-game-spot';

@zlinkSpotActorSendHandler({
  actor: () => PlayActor,
  packetName: 'LeaveGameMsg',
  spot: () => TicTacToeGameSpot
})
class PlayActorLeaveGameHandler
  implements ZLinkSpotActorSendHandler<TicTacToeGameSpot, PlayActor, LeaveGameMsg> {
  constructor(private readonly pendingDestroys: PendingActorDestroyRegistry) {}

  async handle(
    spot: TicTacToeGameSpot,
    actor: PlayActor,
    _context: ZLinkMessageContext,
    request: LeaveGameMsg
  ): Promise<void> {
    if (actor.context.spotId !== request.roomId) {
      throw new Error(`Actor requested leave for a different room. roomId=${request.roomId}`);
    }
    spot.verifyLeave(actor.actorId, request.roomId);
    this.pendingDestroys.mark(actor.actorId);
    await spot.context.leaveActor(actor);
    actor.roomId = undefined;
    console.log(`actor: LeaveGameMsg completed. actor=${actor.actorId}`);
  }
}

export { PlayActorLeaveGameHandler };
