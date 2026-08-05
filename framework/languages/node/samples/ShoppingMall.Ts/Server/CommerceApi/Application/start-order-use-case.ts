import { Injectable } from '@nestjs/common';
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
    const result = await this.workflowRouter.start(this.store.reserveOrder(request));
    return { state: result.state };
  }
}

export { StartOrderUseCase };
