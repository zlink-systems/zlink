import { DecimalAmount, OrderStatuses } from '../../../../Shared/Contracts/messages';
import type { OrderState } from '../../../../Shared/Contracts/messages';
import type { OrderEventType } from '../../../Shared/Domain/order-events';

class OrderAggregate {
  private state?: OrderState;
  private currentEventType?: OrderEventType;

  apply(orderId: string, eventType: OrderEventType, payload: Record<string, any>, occurredAtUnixMs: number): void {
    this.currentEventType = eventType;
    if (eventType === 'OrderStarted') {
      this.state = {
        orderId,
        status: OrderStatuses.Created,
        shippingAddressId: payload.shippingAddressId,
        amount: DecimalAmount.fromWire(payload.amount),
        currency: payload.currency,
        updatedAtUnixMs: occurredAtUnixMs
      };
      return;
    }
    const current = this.requireState();
    if (eventType === 'InventoryReserved') {
      this.state = { ...current, status: OrderStatuses.InventoryReserved, reservationId: payload.reservationId, updatedAtUnixMs: occurredAtUnixMs };
    } else if (eventType === 'PaymentAuthorized') {
      this.state = { ...current, status: OrderStatuses.PaymentAuthorized, paymentId: payload.paymentId, updatedAtUnixMs: occurredAtUnixMs };
    } else if (eventType === 'OrderConfirmed') {
      this.state = { ...current, status: OrderStatuses.Confirmed, updatedAtUnixMs: occurredAtUnixMs };
    } else if (eventType === 'InventoryReservationFailed' || eventType === 'PaymentFailed' || eventType === 'OrderFailed') {
      this.state = { ...current, status: OrderStatuses.Failed, reason: payload.reason, updatedAtUnixMs: occurredAtUnixMs };
    }
  }

  snapshot(): OrderState {
    return { ...this.requireState() };
  }

  lastEventType(): OrderEventType {
    if (this.currentEventType === undefined) throw new Error('Order event stream is empty.');
    return this.currentEventType;
  }

  private requireState(): OrderState {
    if (this.state === undefined) throw new Error('Order event stream does not start with OrderStarted.');
    return this.state;
  }
}

export { OrderAggregate };
