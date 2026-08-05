import { Inject, Injectable } from '@nestjs/common';
import { zlinkSpotPacketHandler } from '@zlink-systems/nestjs';
import type { ZLinkSpotRequestHandler } from '@zlink-systems/framework';
import type {
  VerifyExpectedVersionFenceReq,
  VerifyExpectedVersionFenceRes
} from '../../../../../../Shared/Internal/shoppingmall-workflow-messages';
import { OrderWorkflowService } from '../../../../../Application/OrderWorkflow/order-workflow-service';
import { SHOPPINGMALL_ROLE } from '../../../../../order-workflow-tokens';
import { OrderWorkflowSpot } from '../order-workflow-spot';

@Injectable()
@zlinkSpotPacketHandler({ spot: () => OrderWorkflowSpot, packetName: 'VerifyExpectedVersionFenceReq' })
class VerifyExpectedVersionFenceHandler
  implements ZLinkSpotRequestHandler<
    OrderWorkflowSpot,
    VerifyExpectedVersionFenceReq,
    VerifyExpectedVersionFenceRes
  > {
  constructor(
    private readonly workflow: OrderWorkflowService,
    @Inject(SHOPPINGMALL_ROLE) private readonly role: string
  ) {}

  async handle(
    spot: OrderWorkflowSpot,
    request: VerifyExpectedVersionFenceReq
  ): Promise<VerifyExpectedVersionFenceRes> {
    const result = this.workflow.verifyExpectedVersionFence(request, this.role);
    // Each probe is a separate workflow instance so the next probe can be
    // placed on another process and exercise the event-store version fence.
    const closed = await spot.context.close();
    if (!closed) {
      throw new Error('ShoppingMall expected-version probe could not close its Instance Spot.');
    }
    return result;
  }
}

export { VerifyExpectedVersionFenceHandler };
