import { Injectable } from '@nestjs/common';
import { zlinkSpotPacketHandler } from '@zlink-systems/nestjs';
import type { ZLinkSpotRequestHandler } from '@zlink-systems/framework';
import type { RebuildOrderProjectionReq, RebuildOrderProjectionRes } from '../../../../../../../Shared/Contracts/messages';
import { OrderStatuses } from '../../../../../../../Shared/Contracts/messages';
import { OrderWorkflowService } from '../../../../../Application/OrderWorkflow/order-workflow-service';
import { OrderWorkflowSpot } from '../order-workflow-spot';

@Injectable()
@zlinkSpotPacketHandler({ spot: () => OrderWorkflowSpot, packetName: 'RebuildOrderProjectionReq' })
class RebuildOrderProjectionHandler implements ZLinkSpotRequestHandler<OrderWorkflowSpot, RebuildOrderProjectionReq, RebuildOrderProjectionRes> {
  constructor(private readonly workflow: OrderWorkflowService) {}

  async handle(spot: OrderWorkflowSpot, request: RebuildOrderProjectionReq): Promise<RebuildOrderProjectionRes> {
    const response = this.workflow.rebuild(request);
    if (response.state.status === OrderStatuses.Confirmed || response.state.status === OrderStatuses.Failed) {
      await spot.context.close();
    }
    return response;
  }
}

export { RebuildOrderProjectionHandler };
