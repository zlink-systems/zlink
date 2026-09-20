import { Injectable } from '@nestjs/common';
import { zlinkSpotSubscriptionHandler } from '@zlink-systems/nestjs';
import { SampleNames } from '../../../../../../Configuration/sample-names';
import { BingoRoomSpot } from '../bingo-room-spot';
import type {
  ZLinkPublishMessageContext,
  ZLinkSpotSubscriptionHandler
} from '@zlink-systems/framework';
import type { BingoRewardAcquiredEvent } from '../../../../../../../Shared/Contracts/messages';

// --8<-- [start:doc-bingo-reward-subscribe]
@Injectable()
@zlinkSpotSubscriptionHandler({
  spot: () => BingoRoomSpot,
  channelName: SampleNames.roomRewardChannel,
  topic: SampleNames.roomRewardTopic
})
class BingoRewardAcquiredEventHandler implements ZLinkSpotSubscriptionHandler<
  BingoRoomSpot,
  BingoRewardAcquiredEvent
> {
  async handle(
    room: BingoRoomSpot,
    event: BingoRewardAcquiredEvent,
    context: ZLinkPublishMessageContext
  ): Promise<void> {
    void context;
    await room.announceReward(event);
  }
}
// --8<-- [end:doc-bingo-reward-subscribe]

export { BingoRewardAcquiredEventHandler };
