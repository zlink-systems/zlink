import { Inject } from '@nestjs/common';
import {
  ZLINK_SPOT_OUTBOUND
} from '@zlink-systems/nestjs';
import { questMissionSpotId, SampleNames } from '../../../../Shared/Configuration/sample-names';
import {
  ClosePlayerQuestReq,
  type ClosePlayerQuestRes
} from '../../../../Shared/Contracts/messages';
import {
  ZLinkFrameworkException,
  ZLinkFrameworkErrorKind,
  type ZLinkSpotOutbound
} from '@zlink-systems/framework';

class PlayerQuestSpotProvisioner {
  constructor(
    @Inject(ZLINK_SPOT_OUTBOUND) private readonly outbound: ZLinkSpotOutbound
  ) {}

  async request<TResponse>(playerId: string, request: object): Promise<TResponse> {
    return this.outbound
      .requestToSpot(questMissionSpotId(playerId), request)
      .instanceSpot(SampleNames.playerQuestSpotType)
      .inMesh(SampleNames.playerQuestSpotMesh)
      .submit<TResponse>();
  }

  async send(playerId: string, message: object): Promise<void> {
    await this.outbound
      .sendToSpot(questMissionSpotId(playerId), message)
      .submit();
  }

  async deactivate(playerId: string): Promise<boolean> {
    try {
      // ClosePlayerQuestReq is existing-only. A missing Spot must not be
      // recreated merely to receive a close request. The response also fences
      // lifecycle completion; one-way admission cannot provide that ordering.
      const result = await this.outbound
        .requestToSpot(questMissionSpotId(playerId), new ClosePlayerQuestReq())
        .submit<ClosePlayerQuestRes>();
      return result.closed;
    } catch (error) {
      if (
        error instanceof ZLinkFrameworkException
        && error.kind === ZLinkFrameworkErrorKind.NotFound
      ) {
        return false;
      }
      throw error;
    }
  }
}

export { PlayerQuestSpotProvisioner };
