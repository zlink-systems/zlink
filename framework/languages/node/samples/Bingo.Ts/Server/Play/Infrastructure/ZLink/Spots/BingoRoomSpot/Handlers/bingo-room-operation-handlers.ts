import { Injectable } from '@nestjs/common';
import { zlinkSpotPacketHandler } from '@zlink-systems/nestjs';
import { SubmitBingoCardReq } from '../../../../../../../Shared/Contracts/bingo-messages.generated';
import type {
  ZLinkMessageContext,
  ZLinkSpotRequestHandler
} from '@zlink-systems/framework';
import type { SubmitBingoCardRes } from '../../../../../../../Shared/Contracts/messages';
import { BingoRoomSpot } from '../bingo-room-spot';

class SubmitBingoCardAtSpotReq {
  constructor(readonly actorId: string, readonly request: SubmitBingoCardReq) {}
}

class VerifyStopObservingAtSpotReq {
  constructor(readonly actorId: string, readonly roomId: string) {}
}

@Injectable()
@zlinkSpotPacketHandler({ spot: () => BingoRoomSpot, packetName: 'SubmitBingoCardAtSpotReq' })
class SubmitBingoCardAtSpotHandler
  implements ZLinkSpotRequestHandler<BingoRoomSpot, SubmitBingoCardAtSpotReq, SubmitBingoCardRes> {
  async handle(
    spot: BingoRoomSpot,
    message: SubmitBingoCardAtSpotReq,
    _context: ZLinkMessageContext
  ): Promise<SubmitBingoCardRes> {
    return spot.submitCard(message.actorId, message.request);
  }
}

@Injectable()
@zlinkSpotPacketHandler({ spot: () => BingoRoomSpot, packetName: 'VerifyStopObservingAtSpotReq' })
class VerifyStopObservingAtSpotHandler
  implements ZLinkSpotRequestHandler<BingoRoomSpot, VerifyStopObservingAtSpotReq, {
    readonly stopped: boolean;
  }> {
  async handle(
    spot: BingoRoomSpot,
    message: VerifyStopObservingAtSpotReq,
    _context: ZLinkMessageContext
  ): Promise<{ readonly stopped: boolean }> {
    return {
      stopped: spot.verifyStopObserving(message.actorId, { roomId: message.roomId })
    };
  }
}

export {
  SubmitBingoCardAtSpotHandler,
  SubmitBingoCardAtSpotReq,
  VerifyStopObservingAtSpotHandler,
  VerifyStopObservingAtSpotReq
};
