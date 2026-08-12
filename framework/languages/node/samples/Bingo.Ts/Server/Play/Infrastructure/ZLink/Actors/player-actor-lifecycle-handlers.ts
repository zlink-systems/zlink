import { Injectable } from '@nestjs/common';
import { zlinkSpotActorSendHandler } from '@zlink-systems/nestjs';
import type { ZLinkMessageContext } from '@zlink-systems/framework';
import { PlayerActor } from './player-actor';
import { BingoRoomSpot } from '../Spots/BingoRoomSpot/bingo-room-spot';
import { LeaveFinishedBingoRoomMsg } from '../../../../../Shared/Contracts/bingo-messages.generated';

@Injectable()
@zlinkSpotActorSendHandler({
  spot: () => BingoRoomSpot,
  actor: () => PlayerActor,
  packetName: 'LeaveFinishedBingoRoomMsg'
})
class LeaveFinishedBingoRoomMsgHandler {
  async handle(
    spot: BingoRoomSpot,
    actor: PlayerActor,
    _context: ZLinkMessageContext,
    _message: LeaveFinishedBingoRoomMsg
  ): Promise<void> {
    actor.markForDestroyAfterRoomLeave();
    await spot.context.leaveActor(actor);
  }
}

export {
  LeaveFinishedBingoRoomMsg,
  LeaveFinishedBingoRoomMsgHandler
};
