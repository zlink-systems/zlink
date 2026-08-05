import { zlinkSendHandler } from '@zlink-systems/nestjs';
import { PacketNames } from '../../../Shared/Contracts/messages';
import { DispatchWorker } from '../dispatch-worker';
import type { ZLinkSendHandler, ZLinkMessageContext } from '@zlink-systems/framework';
import type { AssignDeliveryMsg, OfferDeliveryResultMsg } from '../../../Shared/Contracts/messages';

@zlinkSendHandler('dispatch', PacketNames.assignDelivery)
class AssignDeliveryHandler implements ZLinkSendHandler<AssignDeliveryMsg> {
  constructor(private readonly worker: DispatchWorker) {}

  async handle(request: AssignDeliveryMsg, context: ZLinkMessageContext): Promise<void> {
    void context;
    this.worker.assign(request);
    console.error(`deliverydispatch dispatch: queued delivery=${request.deliveryId} customer=${request.customerId}`);
  }
}

@zlinkSendHandler('dispatch', PacketNames.offerDeliveryResult)
class OfferDeliveryResultHandler implements ZLinkSendHandler<OfferDeliveryResultMsg> {
  constructor(private readonly worker: DispatchWorker) {}

  async handle(result: OfferDeliveryResultMsg, context: ZLinkMessageContext): Promise<void> {
    void context;
    this.worker.recordResult(result);
  }
}

export { AssignDeliveryHandler, OfferDeliveryResultHandler };
