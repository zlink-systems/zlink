import { Inject, Injectable } from '@nestjs/common';
import { zlinkSpotPacketHandler } from '@zlink-systems/nestjs';
import type { ZLinkSpotRequestHandler } from '@zlink-systems/framework';
import type { StartOrderWorkflowRes } from '../../../../../../../Shared/Contracts/messages';
import type { PrepareRelocationCheckpointReq } from '../../../../../../Shared/Internal/shoppingmall-workflow-messages';
import { OrderWorkflowService } from '../../../../../Application/OrderWorkflow/order-workflow-service';
import { OrderWorkflowSpot } from '../order-workflow-spot';
import { SHOPPINGMALL_ROLE } from '../../../../../order-workflow-tokens';

@Injectable()
@zlinkSpotPacketHandler({ spot: () => OrderWorkflowSpot, packetName: 'PrepareRelocationCheckpointReq' })
class PrepareRelocationCheckpointHandler implements ZLinkSpotRequestHandler<
  OrderWorkflowSpot,
  PrepareRelocationCheckpointReq,
  StartOrderWorkflowRes & { objectGeneration: string }
> {
  constructor(
    private readonly workflow: OrderWorkflowService,
    @Inject(SHOPPINGMALL_ROLE) private readonly role: string
  ) {}

  handle(
    spot: OrderWorkflowSpot,
    request: PrepareRelocationCheckpointReq
  ): Promise<StartOrderWorkflowRes & { objectGeneration: string }> {
    const response = this.workflow.prepareRelocationCheckpoint(
      request,
      this.role,
      BigInt(spot.context.objectGeneration)
    );
    return Promise.resolve({ ...response, objectGeneration: spot.context.objectGeneration.toString() });
  }
}

export { PrepareRelocationCheckpointHandler };
