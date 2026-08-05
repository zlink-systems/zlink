import { Injectable } from '@nestjs/common';
import { zlinkSpotPacketHandler } from '@zlink-systems/nestjs';
import type {
  ZLinkMessageContext,
  ZLinkSpotRequestHandler
} from '@zlink-systems/framework';
import type { PlaceMarkRes } from '../../../../../../../Shared/Contracts/messages';
import { TicTacToeGameSpot } from '../tictactoe-game-spot';

class PlaceMarkAtGameSpotReq {
  constructor(readonly actorId: string, readonly cell: number) {}
}

class VerifyLeaveGameAtSpotReq {
  constructor(readonly actorId: string, readonly roomId: string) {}
}

@Injectable()
@zlinkSpotPacketHandler({ spot: () => TicTacToeGameSpot, packetName: 'PlaceMarkAtGameSpotReq' })
class PlaceMarkAtGameSpotHandler
  implements ZLinkSpotRequestHandler<TicTacToeGameSpot, PlaceMarkAtGameSpotReq, PlaceMarkRes> {
  async handle(
    spot: TicTacToeGameSpot,
    request: PlaceMarkAtGameSpotReq,
    _context: ZLinkMessageContext
  ): Promise<PlaceMarkRes> {
    return spot.placeMark(request.actorId, request.cell);
  }
}

@Injectable()
@zlinkSpotPacketHandler({ spot: () => TicTacToeGameSpot, packetName: 'VerifyLeaveGameAtSpotReq' })
class VerifyLeaveGameAtSpotHandler
  implements ZLinkSpotRequestHandler<TicTacToeGameSpot, VerifyLeaveGameAtSpotReq, { readonly allowed: true }> {
  async handle(
    spot: TicTacToeGameSpot,
    request: VerifyLeaveGameAtSpotReq,
    _context: ZLinkMessageContext
  ): Promise<{ readonly allowed: true }> {
    spot.verifyLeave(request.actorId, request.roomId);
    return { allowed: true };
  }
}

export {
  PlaceMarkAtGameSpotHandler,
  PlaceMarkAtGameSpotReq,
  VerifyLeaveGameAtSpotHandler,
  VerifyLeaveGameAtSpotReq
};
