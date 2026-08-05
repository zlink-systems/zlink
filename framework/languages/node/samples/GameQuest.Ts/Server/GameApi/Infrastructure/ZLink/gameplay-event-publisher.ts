import { Inject } from '@nestjs/common';
import { ZLINK_SPOT_OUTBOUND } from '@zlink-systems/nestjs';
import { gameplayMsg } from '../../../../Shared/Contracts/messages';
import { questMissionSpotId, SampleNames } from '../../../../Shared/Configuration/sample-names';
import type { ZLinkSpotOutbound } from '@zlink-systems/framework';
import type { GameplayEventEnvelope } from '../../../../Shared/Contracts/messages';

class GameplayEventPublisher {
  constructor(
    @Inject(ZLINK_SPOT_OUTBOUND) private readonly spots: ZLinkSpotOutbound
  ) {}

  async send(event: GameplayEventEnvelope): Promise<void> {
    const message = gameplayMsg(event);
    await this.spots
      .sendToSpot(questMissionSpotId(event.playerId), message)
      .instanceSpot(SampleNames.playerQuestSpotType)
      .inMesh(SampleNames.playerQuestSpotMesh)
      .submit();
  }
}

export { GameplayEventPublisher };
