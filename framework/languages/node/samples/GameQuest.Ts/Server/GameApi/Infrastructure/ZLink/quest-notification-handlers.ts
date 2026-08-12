import { zlinkEntrySpotActorSendHandler } from '@zlink-systems/nestjs';
import {
  DeliverQuestNotificationMsg,
  PacketNames,
  QuestCompletedNotify,
  QuestProgressNotify
} from '../../../../Shared/Contracts/messages';
import { GameQuestEntrySpot } from './gamequest-entry-spot';
import { GameQuestPlayerActor } from './gamequest-player-actor';
import type {
  ZLinkEntrySpotActorSendHandler,
  ZLinkMessageContext
} from '@zlink-systems/framework';

@zlinkEntrySpotActorSendHandler({
  actor: () => GameQuestPlayerActor,
  entrySpot: () => GameQuestEntrySpot,
  packetName: PacketNames.deliverQuestNotificationMsg
})
class DeliverQuestNotificationHandler
  implements ZLinkEntrySpotActorSendHandler<GameQuestEntrySpot, GameQuestPlayerActor, DeliverQuestNotificationMsg> {
  async handle(
    _spot: GameQuestEntrySpot,
    actor: GameQuestPlayerActor,
    _context: ZLinkMessageContext,
    message: DeliverQuestNotificationMsg
  ): Promise<void> {
    if (message.packetName === PacketNames.questCompletedNotify) {
      await actor.push(new QuestCompletedNotify(
        message.playerId,
        message.progress,
        message.rewardGranted ?? false
      ));
      return;
    }
    if (message.packetName !== PacketNames.questProgressNotify) {
      throw new Error(`Unsupported GameQuest notification '${message.packetName}'.`);
    }
    await actor.push(new QuestProgressNotify(message.playerId, message.progress));
  }
}

export { DeliverQuestNotificationHandler };
