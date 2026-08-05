import { zlinkEntrySpotActorSendHandler } from '@zlink-systems/nestjs';
import {
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
  packetName: PacketNames.questProgressNotify
})
class QuestProgressNotificationHandler
  implements ZLinkEntrySpotActorSendHandler<GameQuestEntrySpot, GameQuestPlayerActor, QuestProgressNotify> {
  async handle(
    _spot: GameQuestEntrySpot,
    actor: GameQuestPlayerActor,
    _context: ZLinkMessageContext,
    message: QuestProgressNotify
  ): Promise<void> {
    await actor.push(new QuestProgressNotify(message.playerId, message.progress));
  }
}

@zlinkEntrySpotActorSendHandler({
  actor: () => GameQuestPlayerActor,
  entrySpot: () => GameQuestEntrySpot,
  packetName: PacketNames.questCompletedNotify
})
class QuestCompletedNotificationHandler
  implements ZLinkEntrySpotActorSendHandler<GameQuestEntrySpot, GameQuestPlayerActor, QuestCompletedNotify> {
  async handle(
    _spot: GameQuestEntrySpot,
    actor: GameQuestPlayerActor,
    _context: ZLinkMessageContext,
    message: QuestCompletedNotify
  ): Promise<void> {
    await actor.push(new QuestCompletedNotify(message.playerId, message.progress, message.rewardGranted));
  }
}

export { QuestCompletedNotificationHandler, QuestProgressNotificationHandler };
