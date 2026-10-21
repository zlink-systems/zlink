import { Injectable } from '@nestjs/common';
import { zlinkSpotPacketHandler } from '@zlink-systems/nestjs';
import type { ZLinkMessageContext, ZLinkSpotRequestHandler } from '@zlink-systems/framework';
import {
  GetBingoObservationReq,
  GetBingoObservationRes
} from '../../../../../../../Shared/Contracts/bingo-messages.generated';
import { BingoRoomSpot } from '../bingo-room-spot';

@Injectable()
@zlinkSpotPacketHandler({ spot: () => BingoRoomSpot, packetName: 'GetBingoObservationReq' })
export class GetBingoObservationHandler implements ZLinkSpotRequestHandler<
  BingoRoomSpot,
  GetBingoObservationReq,
  GetBingoObservationRes
> {
  async handle(
    spot: BingoRoomSpot,
    _request: GetBingoObservationReq,
    _context: ZLinkMessageContext
  ): Promise<GetBingoObservationRes> {
    return spot.observation();
  }
}
