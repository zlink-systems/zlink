import { ZLinkPacket } from '@zlink-systems/framework';
import { StartOrderWorkflowReq } from '../../../Shared/Contracts/messages';

// These packets are self-check controls for preparing a recovery fixture. They are not part of
// the application message contract exposed from Shared/Contracts.
@ZLinkPacket('PrepareInventoryReservedReq')
class PrepareInventoryReservedReq extends StartOrderWorkflowReq {}

@ZLinkPacket('PrepareInventoryEffectReq')
class PrepareInventoryEffectReq extends StartOrderWorkflowReq {}

@ZLinkPacket('VerifyExpectedVersionFenceReq')
class VerifyExpectedVersionFenceReq {
  constructor(readonly orderId: string) {}
}

interface VerifyExpectedVersionFenceRes {
  rejected: boolean;
  expectedVersion: number;
  actualVersion: number;
  writerInstanceId?: string;
  ownerInstanceId?: string;
}

export { PrepareInventoryEffectReq, PrepareInventoryReservedReq, VerifyExpectedVersionFenceReq };
export type { VerifyExpectedVersionFenceRes };
