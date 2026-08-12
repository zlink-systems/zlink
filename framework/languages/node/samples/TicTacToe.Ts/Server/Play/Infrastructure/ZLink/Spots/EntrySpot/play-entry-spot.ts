import { Injectable } from '@nestjs/common';
import {
  DeliverPlayNotificationEntryHandler,
  PlayActor
} from '../../Actors/play-actor';
import { MilestoneObserverRegistry } from './entry-spot-registries';
import { PlayActorJoinGameHandler } from './Handlers/play-actor-join-game-handler';
import { PlayActorObserveMilestoneHandler } from './Handlers/play-actor-observe-milestone-handler';
import { PlayerWinMilestoneEventHandler } from './Handlers/player-win-milestone-event-handler';
import { SampleNames } from '../../../../../Configuration/sample-settings';
import type {
  ZLinkActorCreateResponse,
  ZLinkEntrySpot,
  ZLinkEntrySpotContext,
  ZLinkMessage
} from '@zlink-systems/framework';
import {
  PlayerActorCreateReq,
  type PlayerWinMilestoneEvent
} from '../../../../../../Shared/Contracts/messages';

@Injectable()
// --8<-- [start:doc-entry-spot]
class PlayEntrySpot implements ZLinkEntrySpot<PlayActor> {
  readonly context!: ZLinkEntrySpotContext<PlayActor>;

  constructor(
    private readonly milestoneObservers: MilestoneObserverRegistry
  ) {}

  configure(): void {
    // send: JoinGameMsg starts the deferred Room Spot join.
    this.context.handlers.addHandler(PlayActorJoinGameHandler);
    // request: ObserveMilestoneReq returns ObserveMilestoneRes after registration.
    this.context.handlers.addHandler(PlayActorObserveMilestoneHandler);
    // send: the internal notification message is relayed to the current session.
    this.context.handlers.addHandler(DeliverPlayNotificationEntryHandler);
    // subscribe: the published milestone event is delivered to this Entry Spot.
    this.context.handlers.addSubscribe(
      PlayerWinMilestoneEventHandler,
      SampleNames.playerMilestoneChannel,
      SampleNames.playerMilestoneTopic
    );
  }

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
    const { player } = createRequest.decode(PlayerActorCreateReq);
    actor.displayName = player.displayName;
    actor.level = player.level;
    actor.wins = player.wins;
    this.milestoneObservers.track(actor);
    return { accepted: true };
  }

  async onJoinedActor(actor: PlayActor): Promise<void> {
    this.milestoneObservers.track(actor);
    if (actor.destroyAfterEntrySpotJoin) {
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
