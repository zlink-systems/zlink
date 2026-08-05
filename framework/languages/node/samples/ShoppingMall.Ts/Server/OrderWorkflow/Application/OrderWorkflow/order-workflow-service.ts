import { Injectable } from '@nestjs/common';
import { OrderStore } from '../../../Shared/Store/order-store';
import type {
  ContinueOrderWorkflowRes,
  RebuildOrderProjectionRes,
  StartOrderWorkflowReq,
  StartOrderWorkflowRes
} from '../../../../Shared/Contracts/messages';
import type { VerifyExpectedVersionFenceRes } from '../../../Shared/Internal/shoppingmall-workflow-messages';

@Injectable()
class OrderWorkflowService {
  constructor(private readonly store: OrderStore) {}

  start(request: StartOrderWorkflowReq, role: string): StartOrderWorkflowRes {
    const response = this.store.startOrder(request, role);
    return response;
  }

  prepareInventoryReserved(request: StartOrderWorkflowReq, role: string): StartOrderWorkflowRes {
    const response = this.store.prepareInventoryReserved(request, role);
    return response;
  }

  prepareInventoryEffect(request: StartOrderWorkflowReq, role: string): StartOrderWorkflowRes {
    return this.store.prepareInventoryEffect(request, role);
  }

  continue(request: { orderId: string }, role: string): ContinueOrderWorkflowRes {
    const response = this.store.continueOrder(request.orderId, role);
    return response;
  }

  rebuild(request: { orderId: string }): RebuildOrderProjectionRes {
    return this.store.rebuildProjection(request.orderId);
  }

  verifyExpectedVersionFence(request: { orderId: string }, role: string): VerifyExpectedVersionFenceRes {
    return this.store.verifyExpectedVersionFence(request.orderId, role);
  }
}

export { OrderWorkflowService };
