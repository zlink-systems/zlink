import { Injectable } from '@nestjs/common';
import { ZLinkSpotActorRequest, ZLinkSpotActorSend } from '@zlink-systems/framework';
import type {
  ZLinkMessageContext,
  ZLinkSpotActorRequestHandler,
  ZLinkSpotActorSendHandler
} from '@zlink-systems/framework';
import { PlayActor } from '../../../Actors/play-actor';
import type {
  GameState,
  JoinGameMsg,
  PlaceMarkReq,
  PlaceMarkRes
} from '../../../../../../../Shared/Contracts/messages';
import {
  JoinGameFailedNotify,
  joinGameNotify,
  PacketNames
} from '../../../../../../../Shared/Contracts/messages';
import { TicTacToeGameSpot } from '../tictactoe-game-spot';

@Injectable()
class PlayActorCurrentGameStateHandler
  implements ZLinkSpotActorSendHandler<TicTacToeGameSpot, PlayActor, JoinGameMsg> {
  @ZLinkSpotActorSend(PacketNames.joinGameMsg)
  async handle(
    spot: TicTacToeGameSpot,
    actor: PlayActor,
    _context: ZLinkMessageContext,
    message: JoinGameMsg
  ): Promise<void> {
    let state: GameState;
    try {
      state = spot.currentState(actor.actorId, message.roomId);
    } catch (error) {
      await actor.push(new JoinGameFailedNotify(
        message.roomId,
        error instanceof Error ? error.message : String(error)
      ));
      return;
    }
    await actor.push(joinGameNotify(state));
  }
}

@Injectable()
// --8<-- [start:doc-actor-packet-handler]
class PlayActorPlaceMarkHandler
  implements ZLinkSpotActorRequestHandler<TicTacToeGameSpot, PlayActor, PlaceMarkReq, PlaceMarkRes> {
  @ZLinkSpotActorRequest(PacketNames.placeMarkReq)
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

export { PlayActorCurrentGameStateHandler, PlayActorPlaceMarkHandler };
