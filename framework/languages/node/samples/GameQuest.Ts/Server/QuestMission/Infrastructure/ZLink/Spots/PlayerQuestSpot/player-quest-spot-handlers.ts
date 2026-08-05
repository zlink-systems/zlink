import { Inject, Injectable } from '@nestjs/common';
import { zlinkSpotPacketHandler } from '@zlink-systems/nestjs';
import { ClosePlayerQuestMsg } from '../../../../../../Shared/Contracts/messages';
import { QuestEventStore, QuestReadModelStore } from '../../../../../Shared/Store/quest-progress-store';
import { QuestEventProcessor } from '../../../../Application/quest-event-processor';
import { PlayerQuestNotifier } from '../../player-quest-notifier';
import { PlayerQuestSpot } from './player-quest-spot';
import type { ZLinkSpotPacketHandler, ZLinkSpotRequestHandler } from '@zlink-systems/framework';
import type {
  GameplayMsg,
  DeleteQuestProjectionReq,
  DeleteQuestProjectionRes,
  GetQuestProgressReq,
  GetQuestProgressRes,
  QuestProgress,
  RebuildQuestProjectionReq,
  SyncQuestProgressReq,
  SyncQuestProgressRes
} from '../../../../../../Shared/Contracts/messages';

@Injectable()
@zlinkSpotPacketHandler({ spot: () => PlayerQuestSpot, packetName: 'GameplayMsg' })
class ApplyGameplayEventSpotHandler
  implements ZLinkSpotPacketHandler<PlayerQuestSpot, GameplayMsg> {
  constructor(
    private readonly processor: QuestEventProcessor,
    private readonly notifier: PlayerQuestNotifier
  ) {}

  async handle(spot: PlayerQuestSpot, message: GameplayMsg): Promise<void> {
    requirePlayer(spot, message.playerId);
    const aggregate = spot.ensureAggregate(() => this.processor.rehydrate(message.playerId));
    const result = this.processor.process({
      eventId: message.eventId,
      playerId: message.playerId,
      type: message.type,
      payload: message.payload,
      occurredAtUnixMs: message.occurredAtUnixMs
    }, aggregate);
    spot.replaceAggregate(result.aggregate);
    await this.notifier.notify(message.playerId, result.changedProgress, result.completedQuestIds);
  }
}

@Injectable()
@zlinkSpotPacketHandler({ spot: () => PlayerQuestSpot, packetName: 'GetQuestProgressReq' })
class GetQuestProgressSpotHandler
  implements ZLinkSpotRequestHandler<PlayerQuestSpot, GetQuestProgressReq, GetQuestProgressRes> {
  constructor(private readonly processor: QuestEventProcessor) {}

  async handle(spot: PlayerQuestSpot, request: GetQuestProgressReq): Promise<GetQuestProgressRes> {
    requirePlayer(spot, request.playerId);
    return {
      activeQuests: (() => {
        spot.ensureAggregate(() => this.processor.rehydrate(request.playerId));
        return this.processor.readProgress(request.playerId);
      })()
    };
  }
}

@Injectable()
@zlinkSpotPacketHandler({ spot: () => PlayerQuestSpot, packetName: 'SyncQuestProgressReq' })
class SyncQuestProgressSpotHandler
  implements ZLinkSpotRequestHandler<PlayerQuestSpot, SyncQuestProgressReq, SyncQuestProgressRes> {
  constructor(private readonly processor: QuestEventProcessor, private readonly notifier: PlayerQuestNotifier) {}

  async handle(spot: PlayerQuestSpot, request: SyncQuestProgressReq): Promise<SyncQuestProgressRes> {
    requirePlayer(spot, request.playerId);
    const aggregate = spot.ensureAggregate(() => this.processor.rehydrate(request.playerId));
    const result = this.processor.syncProgress(request, aggregate);
    spot.replaceAggregate(result.aggregate);
    await this.notifier.notify(request.playerId, result.changedProgress, result.completedQuestIds);
    return { updatedQuests: result.updatedQuests };
  }
}

@Injectable()
@zlinkSpotPacketHandler({ spot: () => PlayerQuestSpot, packetName: 'DeleteQuestProjectionReq' })
class DeleteQuestProjectionSpotHandler
  implements ZLinkSpotRequestHandler<PlayerQuestSpot, DeleteQuestProjectionReq, DeleteQuestProjectionRes> {
  constructor(@Inject(QuestReadModelStore) private readonly store: QuestReadModelStore) {}

  async handle(spot: PlayerQuestSpot, request: DeleteQuestProjectionReq): Promise<DeleteQuestProjectionRes> {
    requirePlayer(spot, request.playerId);
    this.store.deleteProjection(request.playerId, request.questId);
    return { deleted: true };
  }
}

@Injectable()
@zlinkSpotPacketHandler({ spot: () => PlayerQuestSpot, packetName: 'RebuildQuestProjectionReq' })
class RebuildQuestProjectionSpotHandler
  implements ZLinkSpotRequestHandler<PlayerQuestSpot, RebuildQuestProjectionReq, QuestProgress> {
  constructor(
    @Inject(QuestEventStore) private readonly events: QuestEventStore,
    @Inject(QuestReadModelStore) private readonly store: QuestReadModelStore
  ) {}

  async handle(spot: PlayerQuestSpot, request: RebuildQuestProjectionReq): Promise<QuestProgress> {
    requirePlayer(spot, request.playerId);
    const rebuilt = this.store.rebuildProjection(request.playerId, request.questId, this.events.read(request.playerId));
    return rebuilt;
  }
}

@Injectable()
@zlinkSpotPacketHandler({ spot: () => PlayerQuestSpot, packetName: 'ClosePlayerQuestMsg' })
class ClosePlayerQuestSpotHandler implements ZLinkSpotPacketHandler<PlayerQuestSpot, ClosePlayerQuestMsg> {
  async handle(spot: PlayerQuestSpot, _message: ClosePlayerQuestMsg): Promise<void> {
    await spot.context.close();
  }
}

function requirePlayer(spot: PlayerQuestSpot, playerId: string): void {
  spot.bindPlayer(playerId);
}

export {
  ApplyGameplayEventSpotHandler,
  ClosePlayerQuestSpotHandler,
  DeleteQuestProjectionSpotHandler,
  GetQuestProgressSpotHandler,
  RebuildQuestProjectionSpotHandler,
  SyncQuestProgressSpotHandler
};
