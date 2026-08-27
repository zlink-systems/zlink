import { Injectable } from '@nestjs/common';
import { ZLinkSpotActorSend } from '@zlink-systems/framework';
import type {
  ZLinkMessageContext,
  ZLinkSpotActorSendHandler
} from '@zlink-systems/framework';
import { PlayActor } from '../../../Actors/play-actor';
import type { LeaveGameMsg } from '../../../../../../../Shared/Contracts/messages';
import { PacketNames } from '../../../../../../../Shared/Contracts/messages';
import { TicTacToeGameSpot } from '../tictactoe-game-spot';

@Injectable()
class PlayActorLeaveGameHandler
  implements ZLinkSpotActorSendHandler<TicTacToeGameSpot, PlayActor, LeaveGameMsg> {
  @ZLinkSpotActorSend(PacketNames.leaveGameMsg)
  async handle(
    spot: TicTacToeGameSpot,
    actor: PlayActor,
    _context: ZLinkMessageContext,
    message: LeaveGameMsg
  ): Promise<void> {
    if (actor.context.spotId !== message.roomId) {
      throw new Error(`Actor requested leave for a different room. roomId=${message.roomId}`);
    }
    spot.verifyLeave(actor.actorId, message.roomId);
    actor.markForDestroyAfterRoomLeave();
    await spot.context.leaveActor(actor);
    actor.roomId = undefined;
    console.log(`tictactoe-lifecycle leave-completed actor=${actor.actorId}`);
  }
}

export { PlayActorLeaveGameHandler };
