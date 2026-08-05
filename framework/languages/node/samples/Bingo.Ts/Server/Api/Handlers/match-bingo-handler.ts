import { Inject } from '@nestjs/common';
import { ZLINK_SPOT_MANAGER, ZLINK_SPOT_OUTBOUND, zlinkRequestHandler } from '@zlink-systems/nestjs';
import { PacketNames } from '../../../Shared/Contracts/messages';
import { MatchBingoApiRes, ReserveBingoRoomReq } from '../../../Shared/Contracts/bingo-messages.generated';
import { SampleNames } from '../../Configuration/sample-names';
import type {
  ZLinkSpotManager,
  ZLinkSpotOutbound,
  ZLinkRequestHandler
} from '@zlink-systems/framework';
import type {
  ReserveBingoRoomRes,
  MatchBingoApiReq
} from '../../../Shared/Contracts/messages';

@zlinkRequestHandler('api', PacketNames.matchBingoApiReq)
class MatchBingoHandler implements ZLinkRequestHandler<MatchBingoApiReq, MatchBingoApiRes> {
  constructor(
    @Inject(ZLINK_SPOT_OUTBOUND) private readonly outbound: ZLinkSpotOutbound,
    @Inject(ZLINK_SPOT_MANAGER) private readonly spots: ZLinkSpotManager
  ) {}

  async handle(request: MatchBingoApiReq): Promise<MatchBingoApiRes> {
    const levelBucket = '1-10';
    const allocated = await this.outbound
      .requestToSpot(`match:${levelBucket}`, new ReserveBingoRoomReq({
        mode: request.mode,
        actorId: request.actorId,
        levelBucket
      }))
      .instanceSpot(SampleNames.matchmakerSpotType)
      .inMesh(SampleNames.matchmakingMeshName)
      .submit<ReserveBingoRoomRes>();
    await this.spots
      .getOrCreate(allocated.roomId, SampleNames.roomSpotType)
      .inMesh(SampleNames.playMeshName)
      .request(allocated.settings)
      .submit();
    return new MatchBingoApiRes({ roomId: allocated.roomId });
  }
}

export { MatchBingoHandler };
