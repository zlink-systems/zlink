import { Inject, Injectable } from '@nestjs/common';
import { ZLINK_ACTOR_CLIENT } from '@zlink-systems/nestjs';
import { winMilestoneNotify } from '../../../../../../Shared/Contracts/messages';
import { DeliverPlayNotification, PlayActor } from '../../Actors/play-actor';
import type { ZLinkActorClient } from '@zlink-systems/framework';
import type { PlayerWinMilestoneEvent } from '../../../../../../Shared/Contracts/messages';

@Injectable()
class MilestoneObserverRegistry {
  private readonly actors = new Set<string>();
  private readonly subscriptions = new Set<string>();

  constructor(@Inject(ZLINK_ACTOR_CLIENT) private readonly actorClient: ZLinkActorClient) {}

  track(actor: PlayActor): void {
    this.actors.add(actor.actorId);
  }

  subscribe(actorId: string): void {
    this.subscriptions.add(actorId);
  }

  remove(actorId: string): void {
    this.actors.delete(actorId);
    this.subscriptions.delete(actorId);
  }

  async notify(event: PlayerWinMilestoneEvent): Promise<void> {
    const payload = winMilestoneNotify(event);
    for (const actorId of this.subscriptions) {
      if (this.actors.has(actorId)) {
        await this.actorClient
          .sendToActor(actorId, new DeliverPlayNotification(payload))
          .submit();
      }
    }
  }
}

@Injectable()
class PendingActorDestroyRegistry {
  private readonly actors = new Set<string>();

  mark(actorId: string): void {
    this.actors.add(actorId);
  }

  consume(actorId: string): boolean {
    return this.actors.delete(actorId);
  }
}

export { MilestoneObserverRegistry, PendingActorDestroyRegistry };
