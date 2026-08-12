import { Inject, Injectable } from '@nestjs/common';
import { zlinkSpotPacketHandler } from '@zlink-systems/nestjs';
import type { ZLinkSpotRequestHandler } from '@zlink-systems/framework';
import type { StartOrderWorkflowRes } from '../../../../../../../Shared/Contracts/messages';
import type { PrepareInventoryEffectReq } from '../../../../../../Shared/Internal/shoppingmall-workflow-messages';
import { OrderWorkflowService } from '../../../../../Application/OrderWorkflow/order-workflow-service';
import { OrderWorkflowSpot } from '../order-workflow-spot';
import { SHOPPINGMALL_ROLE } from '../../../../../order-workflow-tokens';

@Injectable()
@zlinkSpotPacketHandler({ spot: () => OrderWorkflowSpot, packetName: 'PrepareInventoryEffectReq' })
class PrepareInventoryEffectHandler implements ZLinkSpotRequestHandler<OrderWorkflowSpot, PrepareInventoryEffectReq, StartOrderWorkflowRes> {
  constructor(
    private readonly workflow: OrderWorkflowService,
    @Inject(SHOPPINGMALL_ROLE) private readonly role: string
  ) {}

  handle(spot: OrderWorkflowSpot, request: PrepareInventoryEffectReq): Promise<StartOrderWorkflowRes> {
    void spot;
    return Promise.resolve(this.workflow.prepareInventoryEffect(request, this.role));
  }
}

export { PrepareInventoryEffectHandler };
