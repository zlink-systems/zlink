import { Inject, Injectable } from '@nestjs/common';
import { zlinkSpotPacketHandler } from '@zlink-systems/nestjs';
import type { ZLinkSpotRequestHandler } from '@zlink-systems/framework';
import type { StartOrderWorkflowReq, StartOrderWorkflowRes } from '../../../../../../../Shared/Contracts/messages';
import { OrderStatuses } from '../../../../../../../Shared/Contracts/messages';
import { OrderWorkflowService } from '../../../../../Application/OrderWorkflow/order-workflow-service';
import { OrderWorkflowSpot } from '../order-workflow-spot';
import { SHOPPINGMALL_ROLE } from '../../../../../order-workflow-tokens';

@Injectable()
@zlinkSpotPacketHandler({ spot: () => OrderWorkflowSpot, packetName: 'StartOrderWorkflowReq' })
class StartOrderWorkflowHandler implements ZLinkSpotRequestHandler<OrderWorkflowSpot, StartOrderWorkflowReq, StartOrderWorkflowRes> {
  constructor(
    private readonly workflow: OrderWorkflowService,
    @Inject(SHOPPINGMALL_ROLE) private readonly role: string
  ) {}

  handle(spot: OrderWorkflowSpot, request: StartOrderWorkflowReq): Promise<StartOrderWorkflowRes> {
    const response = this.workflow.start(request, this.role);
    setImmediate(() => {
      void Promise.resolve()
        .then(() => this.workflow.continue({ orderId: request.orderId }, this.role))
        .then(result => isTerminal(result.state.status) ? spot.context.close() : undefined)
        .catch(error => console.error(
          `shoppingmall order '${request.orderId}' background continuation failed:`,
          error
        ));
    });
    return Promise.resolve(response);
  }
}

function isTerminal(status: string): boolean {
  return status === OrderStatuses.Confirmed || status === OrderStatuses.Failed;
}

export { StartOrderWorkflowHandler };
