type OrderEventType =
  | 'OrderStarted'
  | 'InventoryReserved'
  | 'InventoryReservationFailed'
  | 'PaymentAuthorized'
  | 'PaymentFailed'
  | 'InventoryReleased'
  | 'OrderConfirmed'
  | 'OrderFailed';

interface StoredOrderEvent {
  eventId: string;
  sourceCommandId?: string;
  orderId: string;
  eventType: OrderEventType;
  payload: readonly number[];
  version: number;
  createdAtUnixMs: number;
}

export type { OrderEventType, StoredOrderEvent };
