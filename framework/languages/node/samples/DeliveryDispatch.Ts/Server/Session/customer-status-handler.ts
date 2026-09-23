import { zlinkEntrySpotActorSendHandler } from '@zlink-systems/nestjs';
import { DeliveryStatusNotify, PacketNames } from '../../Shared/Contracts/messages';
import { CustomerActor } from './customer-actor';
import { CustomerEntrySpot } from './customer-entry-spot';
import type { DeliveryStatusUpdatedMsg } from '../../Shared/Contracts/messages';
import type { ZLinkMessageContext } from '@zlink-systems/framework';

@zlinkEntrySpotActorSendHandler({
  entrySpot: () => CustomerEntrySpot,
  actor: () => CustomerActor,
  packetName: PacketNames.deliveryStatusUpdated
})
class CustomerStatusHandler {
  // --8<-- [start:doc-dd-customer-push]
  async handle(
    _spot: CustomerEntrySpot,
    actor: CustomerActor,
    _context: ZLinkMessageContext,
    message: DeliveryStatusUpdatedMsg
  ): Promise<void> {
    if (!actor.accepts(message.deliveryId)) return;
    // --8<-- [start:doc-dd-bound-session-push]
    await actor.context.boundSession
      .send(
        new DeliveryStatusNotify(
          message.deliveryId,
          message.status,
          message.occurredAtUnixMs,
          message.courierId
        )
      )
      .submit();
    // --8<-- [end:doc-dd-bound-session-push]
    if (message.status === 'Delivered') {
      console.log(
        `deliverydispatch-customer pushed status=Delivered delivery=${message.deliveryId}`
      );
    }
  }
  // --8<-- [end:doc-dd-customer-push]
}

export { CustomerStatusHandler };
