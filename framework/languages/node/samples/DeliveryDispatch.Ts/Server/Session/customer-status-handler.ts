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
  async handle(_spot: CustomerEntrySpot, actor: CustomerActor, _context: ZLinkMessageContext, message: DeliveryStatusUpdatedMsg): Promise<void> {
    if (!actor.accepts(message.deliveryId)) return;
    await actor.context.boundSession.send(new DeliveryStatusNotify(
      message.deliveryId,
      message.status,
      message.occurredAtUnixMs,
      message.courierId
    )).submit();
  }
}

export { CustomerStatusHandler };
