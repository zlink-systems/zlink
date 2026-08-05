import { zlinkSpotActorRequestHandler } from '@zlink-systems/nestjs';
import type {
  ZLinkMessageContext,
  ZLinkSpotActorRequestHandler
} from '@zlink-systems/framework';
import { PlayActor } from '../../../Actors/play-actor';
import type { PlaceMarkReq, PlaceMarkRes } from '../../../../../../../Shared/Contracts/messages';
import { TicTacToeGameSpot } from '../tictactoe-game-spot';

@zlinkSpotActorRequestHandler({
  actor: () => PlayActor,
  packetName: 'PlaceMarkReq',
  spot: () => TicTacToeGameSpot
})
// --8<-- [start:doc-actor-packet-handler]
class PlayActorPlaceMarkHandler
  implements ZLinkSpotActorRequestHandler<TicTacToeGameSpot, PlayActor, PlaceMarkReq, PlaceMarkRes> {
  async handle(
    spot: TicTacToeGameSpot,
    actor: PlayActor,
    _context: ZLinkMessageContext,
    request: PlaceMarkReq
  ): Promise<PlaceMarkRes> {
    if (actor.context.spotId === undefined) {
      throw new Error(`Actor '${actor.actorId}' is not joined to a game.`);
    }
    return spot.placeMark(actor.actorId, request.cell);
  }
}
// --8<-- [end:doc-actor-packet-handler]

export { PlayActorPlaceMarkHandler };
