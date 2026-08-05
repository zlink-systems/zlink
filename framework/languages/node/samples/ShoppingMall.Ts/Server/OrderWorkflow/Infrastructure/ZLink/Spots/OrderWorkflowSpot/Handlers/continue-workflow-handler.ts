import { Inject, Injectable } from '@nestjs/common';
import { zlinkSpotPacketHandler } from '@zlink-systems/nestjs';
import type { ZLinkSpotRequestHandler } from '@zlink-systems/framework';
import type { ContinueOrderWorkflowRes } from '../../../../../../../Shared/Contracts/messages';
import { OrderStatuses } from '../../../../../../../Shared/Contracts/messages';
import { OrderWorkflowService } from '../../../../../Application/OrderWorkflow/order-workflow-service';
import { OrderWorkflowSpot } from '../order-workflow-spot';
import { SHOPPINGMALL_ROLE } from '../../../../../order-workflow-tokens';

@Injectable()
@zlinkSpotPacketHandler({ spot: () => OrderWorkflowSpot, packetName: 'ContinueOrderWorkflowReq' })
class ContinueWorkflowHandler implements ZLinkSpotRequestHandler<OrderWorkflowSpot, { orderId: string }, ContinueOrderWorkflowRes> {
  constructor(
    private readonly workflow: OrderWorkflowService,
    @Inject(SHOPPINGMALL_ROLE) private readonly role: string
  ) {}

  async handle(spot: OrderWorkflowSpot, request: { orderId: string }): Promise<ContinueOrderWorkflowRes> {
    const response = this.workflow.continue(request, this.role);
    if (response.state.status === OrderStatuses.Confirmed || response.state.status === OrderStatuses.Failed) {
      await spot.context.close();
    }
    return response;
  }
}

export { ContinueWorkflowHandler };
