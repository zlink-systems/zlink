import type { StartOrderWorkflowReq, StartOrderWorkflowRes } from '../../../Shared/Contracts/messages';
import type { VerifyExpectedVersionFenceRes } from '../../Shared/Internal/shoppingmall-workflow-messages';

interface RelocationCheckpointRes extends StartOrderWorkflowRes {
  objectGeneration: string;
}

abstract class OrderWorkflowRouterPort {
  abstract start(request: StartOrderWorkflowReq): Promise<StartOrderWorkflowRes>;
  abstract prepareInventory(request: StartOrderWorkflowReq): Promise<StartOrderWorkflowRes>;
  abstract prepareRelocationCheckpoint(request: StartOrderWorkflowReq): Promise<RelocationCheckpointRes>;
  abstract prepareInventoryEffect(request: StartOrderWorkflowReq): Promise<StartOrderWorkflowRes>;
  abstract continue(orderId: string): Promise<import('../../../Shared/Contracts/messages').ContinueOrderWorkflowRes>;
  abstract rebuild(orderId: string): Promise<import('../../../Shared/Contracts/messages').RebuildOrderProjectionRes>;
  abstract verifyExpectedVersionFence(orderId: string): Promise<VerifyExpectedVersionFenceRes>;
}

export { OrderWorkflowRouterPort };
export type { RelocationCheckpointRes };
