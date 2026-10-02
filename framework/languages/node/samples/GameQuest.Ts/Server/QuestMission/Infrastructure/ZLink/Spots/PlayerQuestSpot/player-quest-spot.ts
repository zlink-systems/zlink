import { Injectable } from '@nestjs/common';
import { playerIdFromQuestMissionSpotId } from '../../../../../../Shared/Configuration/sample-names';
import { QuestEventProcessor } from '../../../../Application/quest-event-processor';
import type { PlayerQuestAggregate } from '../../../../Domain/quest-domain';
import type { ZLinkInstanceSpot, ZLinkInstanceSpotContext } from '@zlink-systems/framework';

@Injectable()
class PlayerQuestSpot implements ZLinkInstanceSpot {
  readonly context!: ZLinkInstanceSpotContext;
  playerId = '';
  private aggregate: PlayerQuestAggregate | undefined;

  constructor(private readonly processor: QuestEventProcessor) {}

  // --8<-- [start:doc-gq-spot-init]
  // The Instance Spot is addressed by `questMissionSpotId(playerId)`, so the owning
  // player identity is already known at activation. Binding it here (the .NET
  // reference sets `PlayerId = Context.SpotId` in `OnInitializeAsync`) means the Spot
  // never depends on an inbound request field to learn who it belongs to.
  async onInitialize(): Promise<void> {
    this.playerId = playerIdFromQuestMissionSpotId(this.context.spotId);
    this.aggregate = this.processor.rehydrate(this.playerId);
    console.error(
      `gamequest-spot initialize player=${this.playerId} spot=${this.context.spotId} generation=${this.context.objectGeneration}`
    );
  }

  async onClosing(): Promise<void> {
    console.error(
      `gamequest-spot closing-entered player=${this.playerId} generation=${this.context.objectGeneration}`
    );
  }

  bindPlayer(playerId: string): void {
    if (this.playerId === '') {
      this.playerId = playerId;
      return;
    }
    if (this.playerId !== playerId) {
      throw new Error(`Player quest spot '${this.playerId}' cannot process player '${playerId}'.`);
    }
  }

  ensureAggregate(load: () => PlayerQuestAggregate): PlayerQuestAggregate {
    this.aggregate ??= load();
    return this.aggregate;
  }
  // --8<-- [end:doc-gq-spot-init]

  replaceAggregate(aggregate: PlayerQuestAggregate): void {
    this.aggregate = aggregate;
  }
}

export { PlayerQuestSpot };
