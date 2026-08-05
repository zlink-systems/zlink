import { Injectable } from '@nestjs/common';
import { PlayActor } from '../../Actors/play-actor';
import {
  MilestoneObserverRegistry,
  PendingActorDestroyRegistry
} from './entry-spot-registries';
import type {
  ZLinkActorCreateResponse,
  ZLinkEntrySpot,
  ZLinkEntrySpotContext,
  ZLinkMessage
} from '@zlink-systems/framework';
import type { PlayerWinMilestoneEvent, TicTacToeActor } from '../../../../../../Shared/Contracts/messages';

@Injectable()
// --8<-- [start:doc-entry-spot]
class PlayEntrySpot implements ZLinkEntrySpot<PlayActor> {
  readonly context!: ZLinkEntrySpotContext<PlayActor>;

  constructor(
    private readonly milestoneObservers: MilestoneObserverRegistry,
    private readonly pendingDestroys: PendingActorDestroyRegistry
  ) {}

  async onActorJoin(_actorId: string, _request: ZLinkMessage): Promise<{ accepted: boolean }> {
    return { accepted: true };
  }

  async notifyMilestone(event: PlayerWinMilestoneEvent): Promise<void> {
    await this.milestoneObservers.notify(event);
  }

  async onDisconnectActor(actor: PlayActor): Promise<void> {
    this.milestoneObservers.remove(actor.actorId);
  }

  async onCreateActor(actor: PlayActor, createRequest: ZLinkMessage): Promise<ZLinkActorCreateResponse> {
    const player = createRequest.decode<Partial<TicTacToeActor>>(Object as never);
    actor.displayName = typeof player.displayName === 'string'
      ? player.displayName
      : actor.actorId;
    actor.level = typeof player.level === 'number' ? player.level : 0;
    actor.wins = typeof player.wins === 'number' ? player.wins : 0;
    this.milestoneObservers.track(actor);
    return { accepted: true };
  }

  async onJoinedActor(actor: PlayActor): Promise<void> {
    this.milestoneObservers.track(actor);
    if (this.pendingDestroys.consume(actor.actorId)) {
      this.scheduleDestroy(actor);
    }
  }

  async onLeaveActor(actor: PlayActor): Promise<void> {
    console.log(`entry spot: actor left. actor=${actor.actorId}`);
    this.milestoneObservers.remove(actor.actorId);
  }

  scheduleDestroy(actor: PlayActor): void {
    void this.context.runIoWorker(async () => true).submit().then(async () => {
      console.log(`entry spot: actor destroy started. actor=${actor.actorId}`);
      await this.context.destroyActor(actor);
      console.log(`entry spot: actor destroyed. actor=${actor.actorId}`);
    });
  }
}
// --8<-- [end:doc-entry-spot]

export {
  PlayEntrySpot
};
