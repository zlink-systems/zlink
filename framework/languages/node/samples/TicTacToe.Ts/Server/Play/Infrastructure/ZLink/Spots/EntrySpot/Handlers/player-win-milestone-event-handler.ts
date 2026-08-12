import { Injectable } from '@nestjs/common';
import type { ZLinkPublishMessageContext, ZLinkSpotSubscriptionHandler } from '@zlink-systems/framework';
import type { PlayerWinMilestoneEvent } from '../../../../../../../Shared/Contracts/messages';
import { PlayEntrySpot } from '../play-entry-spot';

@Injectable()
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
