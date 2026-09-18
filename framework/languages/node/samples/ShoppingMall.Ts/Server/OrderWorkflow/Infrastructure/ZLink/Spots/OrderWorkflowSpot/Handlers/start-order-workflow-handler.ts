import { Inject, Injectable } from '@nestjs/common';
import { zlinkSpotPacketHandler } from '@zlink-systems/nestjs';
import type { ZLinkSpotRequestHandler } from '@zlink-systems/framework';
import type { StartOrderWorkflowReq, StartOrderWorkflowRes } from '../../../../../../../Shared/Contracts/messages';
import { OrderStatuses } from '../../../../../../../Shared/Contracts/messages';
import { OrderWorkflowService } from '../../../../../Application/OrderWorkflow/order-workflow-service';
import { OrderWorkflowSpot } from '../order-workflow-spot';
import { SHOPPINGMALL_ROLE } from '../../../../../order-workflow-tokens';

// --8<-- [start:doc-sm-start-handler]
@Injectable()
@zlinkSpotPacketHandler({ spot: () => OrderWorkflowSpot, packetName: 'StartOrderWorkflowReq' })
class StartOrderWorkflowHandler implements ZLinkSpotRequestHandler<OrderWorkflowSpot, StartOrderWorkflowReq, StartOrderWorkflowRes> {
  constructor(
    private readonly workflow: OrderWorkflowService,
    @Inject(SHOPPINGMALL_ROLE) private readonly role: string
  ) {}

  // --8<-- [start:doc-sm-spot-start]
  handle(spot: OrderWorkflowSpot, request: StartOrderWorkflowReq): Promise<StartOrderWorkflowRes> {
    // --8<-- [start:doc-sm-background-continue]
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
    // --8<-- [end:doc-sm-background-continue]
  }
  // --8<-- [end:doc-sm-spot-start]
}
// --8<-- [end:doc-sm-start-handler]

function isTerminal(status: string): boolean {
  return status === OrderStatuses.Confirmed || status === OrderStatuses.Failed;
}

export { StartOrderWorkflowHandler };
