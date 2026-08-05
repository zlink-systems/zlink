import { Injectable } from '@nestjs/common';
import { zlinkEntrySpotSubscriptionHandler } from '@zlink-systems/nestjs';
import { SampleNames } from '../../../../../../Configuration/sample-settings';
import type { ZLinkPublishMessageContext, ZLinkSpotSubscriptionHandler } from '@zlink-systems/framework';
import type { PlayerWinMilestoneEvent } from '../../../../../../../Shared/Contracts/messages';
import { PlayEntrySpot } from '../play-entry-spot';

@Injectable()
@zlinkEntrySpotSubscriptionHandler({
  entrySpot: () => PlayEntrySpot,
  channelName: SampleNames.playerMilestoneChannel,
  topic: SampleNames.playerMilestoneTopic
})
class PlayerWinMilestoneEventHandler
  implements ZLinkSpotSubscriptionHandler<PlayEntrySpot, PlayerWinMilestoneEvent> {
  async handle(
    entrySpot: PlayEntrySpot,
    event: PlayerWinMilestoneEvent,
    context: ZLinkPublishMessageContext
  ): Promise<void> {
    void context;
    await entrySpot.notifyMilestone(event);
  }
}

export { PlayerWinMilestoneEventHandler };
