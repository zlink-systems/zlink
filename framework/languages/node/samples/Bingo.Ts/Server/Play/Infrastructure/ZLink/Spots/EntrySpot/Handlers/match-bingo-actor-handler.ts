import { Inject } from '@nestjs/common';
import {
  ZLINK_CHANNEL_CLIENT,
  zlinkEntrySpotActorRequestHandler
} from '@zlink-systems/nestjs';
import { BingoEntrySpot } from '../bingo-entry-spot';
import { PlayerActor } from '../../../Actors/player-actor';
import { BingoRoomStatus, PacketNames } from '../../../../../../../Shared/Contracts/messages';
import {
  BingoRoomJoinReq,
  BingoRoomState,
  MatchBingoApiReq,
  MatchBingoRes
} from '../../../../../../../Shared/Contracts/bingo-messages.generated';
import { SampleNames, SampleTimings } from '../../../../../../Configuration/sample-names';
import type {
  ZLinkChannelClient,
  ZLinkEntrySpotActorRequestHandler,
  ZLinkMessageContext
} from '@zlink-systems/framework';
import type { PlayerActor as PlayerActorType } from '../../../Actors/player-actor';
import type {
  MatchBingoApiRes,
  MatchBingoReq
} from '../../../../../../../Shared/Contracts/messages';

@zlinkEntrySpotActorRequestHandler({
  actor: () => PlayerActor,
  entrySpot: () => BingoEntrySpot,
  packetName: PacketNames.matchBingoReq
})
class MatchBingoActorHandler
  implements ZLinkEntrySpotActorRequestHandler<BingoEntrySpot, PlayerActorType, MatchBingoReq, MatchBingoRes> {
  constructor(
    @Inject(ZLINK_CHANNEL_CLIENT) private readonly channels: ZLinkChannelClient
  ) {}

  async handle(
    _spot: BingoEntrySpot,
    actor: PlayerActorType,
    context: ZLinkMessageContext,
    request: MatchBingoReq
  ): Promise<MatchBingoRes> {
    console.error(`bingo-match request actor=${actor.actorId}`);
    const matched = await this.channels
      .requestToChannel(
        SampleNames.apiChannel,
        new MatchBingoApiReq({
          actorId: actor.actorId,
          displayName: actor.displayName,
          mode: request.mode
        })
      )
      .timeout(SampleTimings.requestTimeout)
      .submit<MatchBingoApiRes>();
    actor.context
      .joinSpot(matched.roomId, new BingoRoomJoinReq({
        roomId: matched.roomId,
        actorId: actor.actorId,
        displayName: actor.displayName,
        observeOnly: false
      }))
      .timeout(SampleTimings.requestTimeout)
      .defer();
    const response = new MatchBingoRes({
      roomId: matched.roomId,
      state: new BingoRoomState({
        roomId: matched.roomId,
        status: BingoRoomStatus.WaitingForPlayers,
        hostActorId: actor.actorId,
        canStart: false,
        drawSeq: 0,
        lastDrawnNumber: null,
        drawnNumbers: [],
        players: [],
        winners: []
      })
    });
    console.error(`bingo-match reply actor=${actor.actorId} room=${response.roomId}`);
    void context;
    return response;
  }
}

export { MatchBingoActorHandler };
