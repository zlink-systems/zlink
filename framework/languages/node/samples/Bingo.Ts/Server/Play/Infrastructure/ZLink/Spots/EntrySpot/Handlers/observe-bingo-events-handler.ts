import { Inject } from '@nestjs/common';
import {
  ZLINK_SPOT_MANAGER,
  zlinkEntrySpotActorRequestHandler
} from '@zlink-systems/nestjs';
import { BingoEntrySpot } from '../bingo-entry-spot';
import { PlayerActor } from '../../../Actors/player-actor';
import { PacketNames } from '../../../../../../../Shared/Contracts/messages';
import type {
  ZLinkEntrySpotActorRequestHandler,
  ZLinkMessageContext,
  ZLinkSpotManager
} from '@zlink-systems/framework';
import type { PlayerActor as PlayerActorType } from '../../../Actors/player-actor';
import type {
  ObserveBingoEventsReq
} from '../../../../../../../Shared/Contracts/messages';
import {
  BingoRoomJoinReq,
  BingoRoomSettingsPayload,
  ObserveBingoEventsRes
} from '../../../../../../../Shared/Contracts/bingo-messages.generated';
import { createObserverRoomSettings } from '../../../../../Domain/Bingo/bingo-room-models';
import { SampleNames } from '../../../../../../Configuration/sample-names';

@zlinkEntrySpotActorRequestHandler({
  actor: () => PlayerActor,
  entrySpot: () => BingoEntrySpot,
  packetName: PacketNames.observeBingoEventsReq
})
class ObserveBingoEventsHandler
  implements ZLinkEntrySpotActorRequestHandler<BingoEntrySpot, PlayerActorType, ObserveBingoEventsReq, ObserveBingoEventsRes> {
  constructor(
    @Inject(ZLINK_SPOT_MANAGER) private readonly spots: ZLinkSpotManager,
  ) {}

  async handle(
    _spot: BingoEntrySpot,
    actor: PlayerActorType,
    context: ZLinkMessageContext,
    request: ObserveBingoEventsReq
  ): Promise<ObserveBingoEventsRes> {
    const observerSpotId = `observe:${request.roomId}:${actor.actorId}`;
    const settings = createObserverRoomSettings(request.roomId, actor.actorId);
    await this.spots
      .getOrCreate(observerSpotId, SampleNames.roomSpotType)
      .inMesh(SampleNames.roomSpotNode)
      .request(new BingoRoomSettingsPayload({
        ...settings,
        purpose: settings.purpose,
        observedRoomId: settings.observedRoomId
      }))
      .submit();
    actor.context
      .joinSpot(observerSpotId, new BingoRoomJoinReq({
        roomId: request.roomId,
        actorId: actor.actorId,
        displayName: actor.displayName,
        observeOnly: true
      }))
      .defer();
    void context;
    return new ObserveBingoEventsRes({
      subscribed: true
    });
  }
}

export { ObserveBingoEventsHandler };
