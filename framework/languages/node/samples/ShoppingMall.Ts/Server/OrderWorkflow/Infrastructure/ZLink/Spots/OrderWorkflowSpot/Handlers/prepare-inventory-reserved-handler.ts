import { Inject, Injectable } from '@nestjs/common';
import { zlinkSpotPacketHandler } from '@zlink-systems/nestjs';
import type { ZLinkSpotRequestHandler } from '@zlink-systems/framework';
import type { StartOrderWorkflowRes } from '../../../../../../../Shared/Contracts/messages';
import type { PrepareInventoryReservedReq } from '../../../../../../Shared/Internal/shoppingmall-workflow-messages';
import { OrderWorkflowService } from '../../../../../Application/OrderWorkflow/order-workflow-service';
import { OrderWorkflowSpot } from '../order-workflow-spot';
import { SHOPPINGMALL_ROLE } from '../../../../../order-workflow-tokens';

@Injectable()
@zlinkSpotPacketHandler({ spot: () => OrderWorkflowSpot, packetName: 'PrepareInventoryReservedReq' })
class PrepareInventoryReservedHandler implements ZLinkSpotRequestHandler<OrderWorkflowSpot, PrepareInventoryReservedReq, StartOrderWorkflowRes> {
  constructor(
    private readonly workflow: OrderWorkflowService,
    @Inject(SHOPPINGMALL_ROLE) private readonly role: string
  ) {}

  handle(spot: OrderWorkflowSpot, request: PrepareInventoryReservedReq): Promise<StartOrderWorkflowRes> {
    void spot;
    return Promise.resolve(this.workflow.prepareInventoryReserved(request, this.role));
  }
}

export { PrepareInventoryReservedHandler };
