import { Inject, Injectable } from '@nestjs/common';
import { ZLINK_ACTOR_MANAGER } from '@zlink-systems/nestjs';
import { SampleNames } from '../../Shared/Configuration/sample-names';
import {
  CustomerActorCreateReq,
  PacketNames,
  SubscribeDeliveryReq,
  SubscribeDeliveryRes
} from '../../Shared/Contracts/messages';
import { CustomerActorDirectory } from './customer-actor';
import {
  ZLinkPacket,
  type ZLinkActorManager,
  type ZLinkMessage,
  type ZLinkSession,
  type ZLinkSessionContext,
  type ZLinkSessionDispatchContext,
  type ZLinkSessionFactory
} from '@zlink-systems/framework';

const CustomerId = 'customer-1';

class CustomerSession implements ZLinkSession {
  constructor(readonly context: ZLinkSessionContext) {}

  async onDispatch(dispatch: ZLinkSessionDispatchContext, payload: ZLinkMessage): Promise<void> {
    if (await this.context.handlers.tryHandle(dispatch, payload)) return;
    const actor = this.context.actors.find(CustomerId);
    if (actor === undefined) throw new Error(`No customer actor is bound for packet '${dispatch.packetName}'.`);
    await actor.relay(payload);
  }
}

@Injectable()
@ZLinkPacket(PacketNames.subscribeDelivery)
class SubscribeDeliverySessionHandler {
  constructor(
    @Inject(ZLINK_ACTOR_MANAGER) private readonly actors: ZLinkActorManager,
    private readonly directory: CustomerActorDirectory
  ) {}

  async handle(context: ZLinkSessionContext, _dispatch: ZLinkSessionDispatchContext, payload: ZLinkMessage): Promise<void> {
    const request = payload.decode(SubscribeDeliveryReq);
    console.error(`deliverydispatch session: find customer delivery=${request.deliveryId}`);
    const ensured = await this.actors
      .getOrCreate(CustomerId, SampleNames.customerActorType)
      .inMesh(SampleNames.customerMeshName)
      .request(new CustomerActorCreateReq(CustomerId))
      .submit();
    if (ensured.status === 'rejected') {
      throw new Error(`Customer actor '${CustomerId}' creation was rejected.`);
    }
    const active = this.directory.require(CustomerId);
    active.subscribe(request.deliveryId);
    if (context.actors.find(ensured.actor.actorId) === undefined) {
      await context.actors.bindOrGet(ensured.actor);
      console.log(`deliverydispatch-customer bound customer=${CustomerId}`);
    }
    context.client.reply(new SubscribeDeliveryRes(request.deliveryId)).submit();
  }
}

class CustomerSessionFactory implements ZLinkSessionFactory<CustomerSession> {
  async create(context: ZLinkSessionContext): Promise<CustomerSession> {
    return new CustomerSession(context);
  }
}

export {
  CustomerSession,
  CustomerSessionFactory,
  SubscribeDeliverySessionHandler
};
