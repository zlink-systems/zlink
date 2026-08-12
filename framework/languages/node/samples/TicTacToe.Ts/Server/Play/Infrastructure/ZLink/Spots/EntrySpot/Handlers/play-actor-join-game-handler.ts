import { Injectable } from '@nestjs/common';
import { ZLinkSpotActorSend } from '@zlink-systems/framework';
import type {
  ZLinkEntrySpotActorSendHandler,
  ZLinkMessageContext
} from '@zlink-systems/framework';
import { PlayActor } from '../../../Actors/play-actor';
import { PlayEntrySpot } from '../play-entry-spot';
import type {
  JoinGameMsg,
  TicTacToeGameJoinReq
} from '../../../../../../../Shared/Contracts/messages';
import { JoinGameFailedNotify, PacketNames } from '../../../../../../../Shared/Contracts/messages';

@Injectable()
// --8<-- [start:doc-join-defer]
class PlayActorJoinGameHandler
  implements ZLinkEntrySpotActorSendHandler<PlayEntrySpot, PlayActor, JoinGameMsg> {
  @ZLinkSpotActorSend(PacketNames.joinGameMsg)
  async handle(
    _spot: PlayEntrySpot,
    actor: PlayActor,
    context: ZLinkMessageContext,
    message: JoinGameMsg
  ): Promise<void> {
    void context;
    const joinRequest: TicTacToeGameJoinReq = {
      roomId: message.roomId,
      player: {
        actorId: actor.actorId,
        displayName: actor.displayName,
        level: actor.level,
        wins: actor.wins
      }
    };
    actor.pendingJoinRoomId = message.roomId;
    try {
      actor.context
        .joinSpot(message.roomId, joinRequest)
        .defer();
    } catch (error) {
      actor.pendingJoinRoomId = undefined;
      await actor.push(new JoinGameFailedNotify(
        message.roomId,
        error instanceof Error ? error.message : String(error)
      ));
    }
  }
}
// --8<-- [end:doc-join-defer]

export { PlayActorJoinGameHandler };
