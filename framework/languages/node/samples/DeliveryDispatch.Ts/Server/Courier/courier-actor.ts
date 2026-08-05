import { Inject, Injectable } from '@nestjs/common';
import { ZLINK_CHANNEL_CLIENT } from '@zlink-systems/nestjs';
import { OfferDeliveryNotify, OfferDeliveryResultMsg } from '../../Shared/Contracts/messages';
import { SampleNames } from '../../Shared/Configuration/sample-names';
import type { BindCourierSessionReq, BindCourierSessionRes, CourierDecisionMsg, OfferDeliveryMsg } from '../../Shared/Contracts/messages';
import type { ZLinkActor, ZLinkActorContext, ZLinkActorFactory, ZLinkChannelClient } from '@zlink-systems/framework';

class CourierActor implements ZLinkActor {
  private readonly offers = new Map<string, number>();

  constructor(
    readonly actorId: string,
    readonly context: ZLinkActorContext,
    private readonly channels: ZLinkChannelClient
  ) {}

  confirmSessionBinding(request: BindCourierSessionReq): BindCourierSessionRes {
    if (request.courierId !== this.actorId) {
      throw new Error(`Courier session '${request.courierId}' does not match actor '${this.actorId}'.`);
    }
    return { courierId: this.actorId };
  }

  async offer(request: OfferDeliveryMsg): Promise<void> {
    this.offers.set(request.deliveryId, request.attempt);
    await this.context.boundSession.send(new OfferDeliveryNotify(
      request.courierId,
      request.deliveryId,
      request.pickupAddress,
      request.dropoffAddress
    )).submit();
  }

  async decide(decision: CourierDecisionMsg): Promise<void> {
    const attempt = this.offers.get(decision.deliveryId);
    if (attempt === undefined) {
      throw new Error(`Courier actor '${this.actorId}' has no active offer for '${decision.deliveryId}'.`);
    }
    await this.channels.sendToChannel(
      SampleNames.dispatchChannel,
      new OfferDeliveryResultMsg(
        decision.deliveryId,
        decision.courierId,
        attempt,
        decision.accepted,
        decision.reason
      )
    ).submit();
  }

}

class CourierActorDirectory {
  private readonly actors = new Map<string, CourierActor>();
  add(actor: CourierActor): void { this.actors.set(actor.actorId, actor); }
  require(actorId: string): CourierActor {
    const actor = this.actors.get(actorId);
    if (actor === undefined) throw new Error(`Courier actor '${actorId}' is not active.`);
    return actor;
  }
}

@Injectable()
class CourierActorFactory implements ZLinkActorFactory {
  constructor(
    private readonly directory: CourierActorDirectory,
    @Inject(ZLINK_CHANNEL_CLIENT) private readonly channels: ZLinkChannelClient
  ) {}

  async create(context: ZLinkActorContext): Promise<CourierActor> {
    const actor = new CourierActor(context.actorId, context, this.channels);
    this.directory.add(actor);
    return actor;
  }
}

export {
  CourierActor,
  CourierActorFactory,
  CourierActorDirectory
};
