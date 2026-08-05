import { zlinkSpotActorRequestHandler } from '@zlink-systems/nestjs';
import { BingoRoomSpot } from '../bingo-room-spot';
import { PlayerActor } from '../../../Actors/player-actor';
import { PacketNames } from '../../../../../../../Shared/Contracts/messages';
import { StopObservingBingoEventsRes } from '../../../../../../../Shared/Contracts/bingo-messages.generated';
import type {
  ZLinkMessageContext,
  ZLinkSpotActorRequestHandler
} from '@zlink-systems/framework';
import type { StopObservingBingoEventsReq } from '../../../../../../../Shared/Contracts/messages';

@zlinkSpotActorRequestHandler({
  actor: () => PlayerActor,
  spot: () => BingoRoomSpot,
  packetName: PacketNames.stopObservingBingoEventsReq
})
class StopObservingBingoEventsHandler
  implements ZLinkSpotActorRequestHandler<BingoRoomSpot, PlayerActor, StopObservingBingoEventsReq, StopObservingBingoEventsRes> {
  async handle(
    spot: BingoRoomSpot,
    actor: PlayerActor,
    _context: ZLinkMessageContext,
    request: StopObservingBingoEventsReq
  ): Promise<StopObservingBingoEventsRes> {
    if (!spot.verifyStopObserving(actor.actorId, request)) {
      return new StopObservingBingoEventsRes({ stopped: false });
    }
    await spot.context.leaveActor(actor);
    return new StopObservingBingoEventsRes({ stopped: true });
  }
}

export { StopObservingBingoEventsHandler };
