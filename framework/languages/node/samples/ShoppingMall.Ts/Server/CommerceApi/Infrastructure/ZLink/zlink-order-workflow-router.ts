import { Injectable } from '@nestjs/common';
import type { ZLinkSpotOutbound } from '@zlink-systems/framework';
import { ZLINK_SPOT_OUTBOUND } from '@zlink-systems/nestjs';
import { Inject } from '@nestjs/common';
import { SampleNames } from '../../../../Shared/Configuration/sample-names';
import { OrderWorkflowRouterPort } from '../../Application/order-workflow-router-port';
import {
  ContinueOrderWorkflowReq,
  RebuildOrderProjectionReq,
  StartOrderWorkflowReq,
} from '../../../../Shared/Contracts/messages';
import {
  PrepareInventoryEffectReq,
  PrepareInventoryReservedReq,
  VerifyExpectedVersionFenceReq
} from '../../../Shared/Internal/shoppingmall-workflow-messages';
import type { VerifyExpectedVersionFenceRes } from '../../../Shared/Internal/shoppingmall-workflow-messages';
import type {
  ContinueOrderWorkflowRes,
  RebuildOrderProjectionRes,
  StartOrderWorkflowRes
} from '../../../../Shared/Contracts/messages';

@Injectable()
class ZLinkOrderWorkflowRouter implements OrderWorkflowRouterPort {
  constructor(@Inject(ZLINK_SPOT_OUTBOUND) private readonly spots: ZLinkSpotOutbound) {}

  start(request: StartOrderWorkflowReq): Promise<StartOrderWorkflowRes> {
    return this.request(request);
  }

  prepareInventory(request: StartOrderWorkflowReq): Promise<StartOrderWorkflowRes> {
    return this.request(new PrepareInventoryReservedReq(
      request.orderId,
      request.cartId,
      request.shippingAddressId,
      request.paymentMethodId,
      request.idempotencyKey,
      request.sourceCommandId,
      request.lines,
      request.amount,
      request.currency
    ));
  }

  prepareInventoryEffect(request: StartOrderWorkflowReq): Promise<StartOrderWorkflowRes> {
    return this.request(new PrepareInventoryEffectReq(
      request.orderId, request.cartId, request.shippingAddressId, request.paymentMethodId,
      request.idempotencyKey, request.sourceCommandId, request.lines, request.amount, request.currency
    ));
  }

  continue(orderId: string): Promise<ContinueOrderWorkflowRes> {
    return this.request(new ContinueOrderWorkflowReq(orderId, `continue-${orderId}`));
  }

  rebuild(orderId: string): Promise<RebuildOrderProjectionRes> {
    return this.request(new RebuildOrderProjectionReq(orderId, `rebuild-${orderId}`));
  }

  async verifyExpectedVersionFence(orderId: string): Promise<VerifyExpectedVersionFenceRes> {
    for (let attempt = 0; attempt < 16; attempt++) {
      const result = await this.request<VerifyExpectedVersionFenceRes>(new VerifyExpectedVersionFenceReq(orderId));
      if (result.rejected) return result;
    }
    throw new Error(`Expected-version fence was not exercised by a second workflow instance for '${orderId}'.`);
  }

  private request<TResponse>(payload: object): Promise<TResponse> {
    const orderId = requireOrderId(payload);
    return this.spots
      .requestToSpot(orderId, payload)
      .instanceSpot(SampleNames.orderWorkflowSpotType)
      .inMesh(SampleNames.orderWorkflowSpotMesh)
      .timeout(SampleNames.requestTimeout)
      .submit<TResponse>();
  }

}

function requireOrderId(payload: object): string {
  const orderId = (payload as { readonly orderId?: unknown }).orderId;
  if (typeof orderId !== 'string' || orderId.length === 0) {
    throw new Error('Order workflow request requires a non-empty orderId.');
  }
  return orderId;
}

export { ZLinkOrderWorkflowRouter };
