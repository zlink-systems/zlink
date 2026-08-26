import { Inject } from '@nestjs/common';
import {
  ZLINK_SPOT_OUTBOUND
} from '@zlink-systems/nestjs';
import { questMissionSpotId, SampleNames } from '../../../../Shared/Configuration/sample-names';
import {
  ClosePlayerQuestMsg
} from '../../../../Shared/Contracts/messages';
import type { ZLinkSpotOutbound } from '@zlink-systems/framework';

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
    // The close is intentionally one-way: the next instance intent observes
    // the completed lifecycle and creates the generation that replays state.
    await this.send(playerId, new ClosePlayerQuestMsg());
    return true;
  }
}

export { PlayerQuestSpotProvisioner };
