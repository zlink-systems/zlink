import { Inject } from '@nestjs/common';
import {
  ZLINK_ACTOR_CLIENT,
  zlinkRequestHandler
} from '@zlink-systems/nestjs';
import { DeliveryStatusChangedReq, DeliveryStatusUpdatedMsg, PacketNames } from '../../../Shared/Contracts/messages';
import { EvidenceStore } from '../../Configuration/evidence-store';
import type {
  ZLinkActorClient,
  ZLinkMessageContext,
  ZLinkRequestHandler
} from '@zlink-systems/framework';
import type { DeliveryStatusChangedRes } from '../../../Shared/Contracts/messages';

@zlinkRequestHandler('tracking', PacketNames.deliveryStatusChanged)
class DeliveryStatusChangedHandler implements ZLinkRequestHandler<DeliveryStatusChangedReq, DeliveryStatusChangedRes> {
  constructor(
    @Inject(ZLINK_ACTOR_CLIENT) private readonly actors: ZLinkActorClient,
    private readonly evidence: EvidenceStore
  ) {}

  async handle(request: DeliveryStatusChangedReq, context: ZLinkMessageContext): Promise<DeliveryStatusChangedRes> {
    void context;
    this.evidence.append(request);
    await this.actors.sendToActor(request.customerId, new DeliveryStatusUpdatedMsg(
      request.deliveryId,
      request.customerId,
      request.status,
      request.occurredAtUnixMs,
      request.courierId
    )).submit();
    console.error(`deliverydispatch tracking: status delivery=${request.deliveryId} status=${request.status} courier=${request.courierId ?? '-'}`);
    return { deliveryId: request.deliveryId, status: request.status };
  }
}

export {
  DeliveryStatusChangedHandler
};
