import { Injectable } from '@nestjs/common';
import { ZLinkFrameworkErrorKind, ZLinkFrameworkException } from '@zlink-systems/framework';
import type { StartOrderReq, StartOrderRes } from '../../../Shared/Contracts/messages';
import { OrderWorkflowRouterPort } from './order-workflow-router-port';
import { OrderStore } from '../../Shared/Store/order-store';

@Injectable()
class StartOrderUseCase {
  constructor(
    private readonly workflowRouter: OrderWorkflowRouterPort,
    private readonly store: OrderStore
  ) {}

  async start(request: StartOrderReq): Promise<StartOrderRes> {
    const workflowRequest = this.store.reserveOrder(request);
    try {
      const result = await this.workflowRouter.start(workflowRequest);
      return { state: result.state };
    } catch (error) {
      if (
        error instanceof ZLinkFrameworkException &&
        error.kind === ZLinkFrameworkErrorKind.Rejected
      ) {
        const state = this.store.getOrderByIdempotencyKey(request.idempotencyKey);
        if (state !== undefined) return { state };
      }
      throw error;
    }
  }
}

export { StartOrderUseCase };
