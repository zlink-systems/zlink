import type { PlayerQuestAggregate } from '../../../../Domain/quest-domain';
import type { ZLinkInstanceSpot, ZLinkInstanceSpotContext } from '@zlink-systems/framework';

class PlayerQuestSpot implements ZLinkInstanceSpot {
  readonly context!: ZLinkInstanceSpotContext;
  playerId = '';
  private aggregate: PlayerQuestAggregate | undefined;

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

  replaceAggregate(aggregate: PlayerQuestAggregate): void {
    this.aggregate = aggregate;
  }
}

export { PlayerQuestSpot };
