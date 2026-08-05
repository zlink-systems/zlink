import { ZLinkPacket } from '@zlink-systems/framework';
import { DecimalAmount } from './decimal-amount';
import type { DecimalAmountInput } from './decimal-amount';

const PacketNames = {
  startOrderWorkflowReq: 'StartOrderWorkflowReq',
  continueOrderWorkflowReq: 'ContinueOrderWorkflowReq',
  rebuildOrderProjectionReq: 'RebuildOrderProjectionReq'
} as const;

interface OrderLineInput {
  sku: string;
  quantity: number;
}

interface StartOrderReq {
  cartId: string;
  shippingAddressId: string;
  paymentMethodId: string;
  idempotencyKey: string;
}

@ZLinkPacket(PacketNames.startOrderWorkflowReq)
class StartOrderWorkflowReq {
  readonly amount: DecimalAmount;

  constructor(
    readonly orderId: string,
    readonly cartId: string,
    readonly shippingAddressId: string,
    readonly paymentMethodId: string,
    readonly idempotencyKey: string,
    readonly sourceCommandId: string,
    readonly lines: readonly OrderLineInput[],
    amount: DecimalAmountInput,
    readonly currency: string
  ) {
    this.amount = DecimalAmount.fromWire(amount);
  }
}

@ZLinkPacket(PacketNames.continueOrderWorkflowReq)
class ContinueOrderWorkflowReq {
  constructor(readonly orderId: string, readonly sourceCommandId: string) {}
}

@ZLinkPacket(PacketNames.rebuildOrderProjectionReq)
class RebuildOrderProjectionReq {
  constructor(readonly orderId: string, readonly sourceCommandId: string) {}
}

interface StartOrderRes {
  state: OrderState;
}

interface StartOrderWorkflowRes { state: OrderState; }
interface GetOrderStateRes { state: OrderState; }

interface OrderState {
  orderId: string;
  status: OrderStatus;
  shippingAddressId?: string;
  reservationId?: string;
  paymentId?: string;
  reason?: string;
  amount?: DecimalAmount;
  currency?: string;
  updatedAtUnixMs: number;
}

interface ContinueOrderWorkflowRes { state: OrderState; }
interface RebuildOrderProjectionRes { state: OrderState; }
interface ServerAssertionReq {
  successfulOrderId: string;
  pendingRecoveredOrderId: string;
  concurrentOrderId: string;
  resumedOrderId: string;
  interruptedOrderId: string;
  inventoryFailureOrderId: string;
  paymentFailureOrderId: string;
  scaleOutOrderIds: readonly string[];
}

interface ServerAssertionRes {
  passed: boolean;
  evidence: string[];
}

const OrderStatuses = {
  Created: 'Created',
  InventoryReserved: 'InventoryReserved',
  PaymentAuthorized: 'PaymentAuthorized',
  Confirmed: 'Confirmed',
  Failed: 'Failed'
} as const;

type OrderStatus = typeof OrderStatuses[keyof typeof OrderStatuses];

export {
  ContinueOrderWorkflowReq,
  DecimalAmount,
  OrderStatuses,
  PacketNames,
  RebuildOrderProjectionReq,
  StartOrderWorkflowReq,
};
export type {
  ContinueOrderWorkflowRes,
  GetOrderStateRes,
  OrderLineInput,
  OrderStatus,
  OrderState,
  RebuildOrderProjectionRes,
  ServerAssertionReq,
  ServerAssertionRes,
  StartOrderReq,
  StartOrderRes,
  StartOrderWorkflowRes
};
