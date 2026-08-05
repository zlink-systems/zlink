import { Inject } from '@nestjs/common';
import { ZLINK_ACTOR_CLIENT, ZLINK_ACTOR_MANAGER } from '@zlink-systems/nestjs';
import {
  QuestCompletedNotify,
  QuestProgressNotify
} from '../../../../Shared/Contracts/messages';
import type {
  ZLinkActorClient,
  ZLinkActorManager
} from '@zlink-systems/framework';
import type { QuestProgress } from '../../../../Shared/Contracts/messages';

class PlayerQuestNotifier {
  constructor(
    @Inject(ZLINK_ACTOR_MANAGER) private readonly actorManager: ZLinkActorManager,
    @Inject(ZLINK_ACTOR_CLIENT) private readonly actors: ZLinkActorClient
  ) {}

  async notify(playerId: string, progress: QuestProgress[], completedQuestIds: string[]): Promise<void> {
    if (progress.length === 0) return;
    const actor = await this.actorManager.find(playerId);
    if (actor === undefined) {
      console.error(`gamequest notification skipped: no bound actor location player=${playerId}`);
      return;
    }
    for (const changed of progress) {
      await this.actors.sendToActor(
        actor.actorId,
        new QuestProgressNotify(playerId, changed)
      ).submit();
      if (completedQuestIds.includes(changed.questId)) {
        await this.actors.sendToActor(
          actor.actorId,
          new QuestCompletedNotify(playerId, changed, true)
        ).submit();
      }
    }
  }
}

export { PlayerQuestNotifier };
