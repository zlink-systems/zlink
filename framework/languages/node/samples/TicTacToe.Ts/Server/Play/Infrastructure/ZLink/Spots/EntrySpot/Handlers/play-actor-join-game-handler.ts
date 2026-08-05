import { zlinkEntrySpotActorRequestHandler } from '@zlink-systems/nestjs';
import type {
  ZLinkEntrySpotActorRequestHandler,
  ZLinkMessageContext
} from '@zlink-systems/framework';
import { PlayActor } from '../../../Actors/play-actor';
import { PlayEntrySpot } from '../play-entry-spot';
import type {
  JoinGameReq,
  JoinGameRes,
  TicTacToeGameJoinReq
} from '../../../../../../../Shared/Contracts/messages';
import { GameStatus, joinGameRes } from '../../../../../../../Shared/Contracts/messages';

@zlinkEntrySpotActorRequestHandler({
  actor: () => PlayActor,
  entrySpot: () => PlayEntrySpot,
  packetName: 'JoinGameReq'
})
// --8<-- [start:doc-join-defer]
class PlayActorJoinGameHandler
  implements ZLinkEntrySpotActorRequestHandler<PlayEntrySpot, PlayActor, JoinGameReq, JoinGameRes> {
  async handle(
    _spot: PlayEntrySpot,
    actor: PlayActor,
    context: ZLinkMessageContext,
    request: JoinGameReq
  ): Promise<JoinGameRes> {
    void context;
    const joinRequest: TicTacToeGameJoinReq = {
      roomId: request.roomId,
      player: {
        actorId: actor.actorId,
        displayName: actor.displayName,
        level: actor.level,
        wins: actor.wins
      }
    };
    actor.context
      .joinSpot(request.roomId, joinRequest)
      .defer();
    actor.roomId = request.roomId;
    return joinGameRes({
      roomId: request.roomId,
      board: '.........',
      status: GameStatus.WaitingForPlayers,
      winner: null,
      nextTurn: '',
      xActorId: null,
      oActorId: null,
      lastMoveActorId: null,
      lastMoveCell: null
    });
  }
}
// --8<-- [end:doc-join-defer]

export { PlayActorJoinGameHandler };
