import type { VerifyExpectedVersionFenceRes } from '../../Shared/Internal/shoppingmall-workflow-messages';

abstract class OrderWorkflowRouterPort {
  abstract start(request: import('../../../Shared/Contracts/messages').StartOrderWorkflowReq): Promise<import('../../../Shared/Contracts/messages').StartOrderWorkflowRes>;
  abstract prepareInventory(request: import('../../../Shared/Contracts/messages').StartOrderWorkflowReq): Promise<import('../../../Shared/Contracts/messages').StartOrderWorkflowRes>;
  abstract prepareInventoryEffect(request: import('../../../Shared/Contracts/messages').StartOrderWorkflowReq): Promise<import('../../../Shared/Contracts/messages').StartOrderWorkflowRes>;
  abstract continue(orderId: string): Promise<import('../../../Shared/Contracts/messages').ContinueOrderWorkflowRes>;
  abstract rebuild(orderId: string): Promise<import('../../../Shared/Contracts/messages').RebuildOrderProjectionRes>;
  abstract verifyExpectedVersionFence(orderId: string): Promise<VerifyExpectedVersionFenceRes>;
}

export { OrderWorkflowRouterPort };
