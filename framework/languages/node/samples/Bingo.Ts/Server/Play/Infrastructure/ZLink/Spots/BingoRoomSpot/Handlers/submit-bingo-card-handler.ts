import { zlinkSpotActorRequestHandler } from '@zlink-systems/nestjs';
import { BingoRoomSpot } from '../bingo-room-spot';
import { PlayerActor } from '../../../Actors/player-actor';
import { PacketNames } from '../../../../../../../Shared/Contracts/messages';
import type {
  ZLinkMessageContext,
  ZLinkSpotActorRequestHandler
} from '@zlink-systems/framework';
import type {
  SubmitBingoCardReq,
  SubmitBingoCardRes
} from '../../../../../../../Shared/Contracts/messages';

@zlinkSpotActorRequestHandler({
  actor: () => PlayerActor,
  packetName: PacketNames.submitBingoCardReq,
  spot: () => BingoRoomSpot
})
class SubmitBingoCardHandler
  implements ZLinkSpotActorRequestHandler<BingoRoomSpot, PlayerActor, SubmitBingoCardReq, SubmitBingoCardRes> {
  async handle(
    spot: BingoRoomSpot,
    actor: PlayerActor,
    _context: ZLinkMessageContext,
    request: SubmitBingoCardReq
  ): Promise<SubmitBingoCardRes> {
    return spot.submitCard(actor.actorId, request);
  }
}

export { SubmitBingoCardHandler };
