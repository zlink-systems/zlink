import { DecimalAmount, OrderStatuses } from '../Shared/Contracts/messages';
import type { ZLinkHttpClient } from '@zlink-systems/http-client';
import { zlinkStreamAssert } from '@zlink-systems/stream-connector';
import type {
  ContinueOrderWorkflowRes,
  GetOrderStateRes,
  OrderState,
  RebuildOrderProjectionRes,
  StartOrderReq,
  StartOrderRes
} from '../Shared/Contracts/messages';
import type { ShoppingMallSampleConfig } from './Configuration/sample-config';

class ShoppingMallClientScenario {
  async run(
    apiA: ZLinkHttpClient,
    apiB: ZLinkHttpClient,
    fixtures: ShoppingMallSampleConfig,
    signal?: AbortSignal
  ): Promise<void> {
    const successReq = startOrderReq('cart-success', 'addr-home', 'pm-ok', 'order-success-001');
    const success = await apiA.post('/orders/start').body(successReq).fetch<StartOrderRes>();
    zlinkStreamAssert.ensure(success.state.status === OrderStatuses.Created, 'Sample scenario assertion failed.');
    zlinkStreamAssert.ensure(success.state.orderId.length > 0, 'Sample scenario assertion failed.');

    const created = await this.getOrder(apiA, success.state.orderId);
    zlinkStreamAssert.ensure(this.isStartedOrConfirmed(created), 'Sample scenario assertion failed.');
    zlinkStreamAssert.ensure(created.shippingAddressId === successReq.shippingAddressId, 'Sample scenario assertion failed.');

    const confirmed = await this.waitForStatus(apiA, success.state.orderId, OrderStatuses.Confirmed, signal);
    zlinkStreamAssert.ensure(confirmed.reservationId === `reservation-${success.state.orderId}`, 'Sample scenario assertion failed.');
    zlinkStreamAssert.ensure(confirmed.paymentId === `payment-${success.state.orderId}`, 'Sample scenario assertion failed.');
    zlinkStreamAssert.ensure(
      DecimalAmount.fromMinorUnits(12_000n).equalsWire(confirmed.amount),
      'Sample scenario assertion failed.'
    );
    zlinkStreamAssert.ensure(confirmed.currency === 'USD', 'Sample scenario assertion failed.');
    reportOrder('success', success.state.orderId);

    const duplicate = await apiB.post('/orders/start').body(successReq).fetch<StartOrderRes>();
    zlinkStreamAssert.ensure(duplicate.state.orderId === success.state.orderId, 'Sample scenario assertion failed.');

    const concurrentReq = startOrderReq('cart-success', 'addr-office', 'pm-ok', 'order-concurrent-001');
    const [concurrentA, concurrentB] = await Promise.all([
      this.startWithRetry(apiA, concurrentReq, signal),
      this.startWithRetry(apiB, concurrentReq, signal)
    ]);
    zlinkStreamAssert.ensure(concurrentA.state.orderId === concurrentB.state.orderId, 'Sample scenario assertion failed.');
    const concurrentConfirmed = await this.waitForStatus(apiA, concurrentA.state.orderId, OrderStatuses.Confirmed, signal);
    zlinkStreamAssert.ensure(concurrentConfirmed.status === OrderStatuses.Confirmed, 'Sample scenario assertion failed.');
    reportOrder('concurrent', concurrentA.state.orderId);

    const pending = await apiB.post('/orders/start').body(
      startOrderReq('cart-success', 'addr-office', 'pm-ok', fixtures.pendingIdempotencyKey)
    ).fetch<StartOrderRes>();
    zlinkStreamAssert.ensure(pending.state.orderId === fixtures.pendingOrderId, 'Sample scenario assertion failed.');
    const pendingConfirmed = await this.waitForStatus(apiA, pending.state.orderId, OrderStatuses.Confirmed, signal);
    zlinkStreamAssert.ensure(pendingConfirmed.status === OrderStatuses.Confirmed, 'Sample scenario assertion failed.');
    reportOrder('pending', pending.state.orderId);

    await this.assertContinued(apiB, fixtures.resumedOrderId, signal);
    reportOrder('resumed', fixtures.resumedOrderId);
    await this.assertContinued(apiA, fixtures.interruptedOrderId, signal);
    reportOrder('interrupted', fixtures.interruptedOrderId);
    if (fixtures.relocatedOrderId !== undefined) {
      await this.assertContinued(apiB, fixtures.relocatedOrderId, signal);
      reportOrder('relocated', fixtures.relocatedOrderId);
    }

    const inventoryReq = startOrderReq('cart-inventory-fail', 'addr-home', 'pm-ok', 'order-inventory-001');
    const inventoryStarted = await apiA.post('/orders/start').body(inventoryReq).fetch<StartOrderRes>();
    const inventoryFailed = await this.waitForStatus(apiA, inventoryStarted.state.orderId, OrderStatuses.Failed, signal);
    zlinkStreamAssert.ensure(inventoryFailed.reason?.toLowerCase().includes('inventory') === true, 'Sample scenario assertion failed.');
    reportOrder('inventory-failure', inventoryStarted.state.orderId);

    const paymentReq = startOrderReq('cart-payment-fail', 'addr-home', 'pm-decline', 'order-payment-001');
    const paymentStarted = await apiB.post('/orders/start').body(paymentReq).fetch<StartOrderRes>();
    const paymentFailed = await this.waitForStatus(apiB, paymentStarted.state.orderId, OrderStatuses.Failed, signal);
    zlinkStreamAssert.ensure(paymentFailed.reservationId !== undefined, 'Sample scenario assertion failed.');
    zlinkStreamAssert.ensure(paymentFailed.reason?.toLowerCase().includes('payment') === true, 'Sample scenario assertion failed.');
    reportOrder('payment-failure', paymentStarted.state.orderId);

    const rebuilt = await apiA.post(`/orders/${encodeURIComponent(fixtures.rebuiltOrderId)}/rebuild`)
      .fetch<RebuildOrderProjectionRes>();
    zlinkStreamAssert.ensure(rebuilt.state.status === OrderStatuses.Confirmed, 'Sample scenario assertion failed.');
    const rebuiltRead = await this.getOrder(apiB, fixtures.rebuiltOrderId);
    zlinkStreamAssert.ensure(rebuiltRead.status === OrderStatuses.Confirmed, 'Sample scenario assertion failed.');
    reportOrder('rebuilt', fixtures.rebuiltOrderId);

    const [scaleA, scaleB] = await Promise.all([
      apiA.post('/orders/start')
        .body(startOrderReq('cart-success', 'addr-office', 'pm-ok', 'order-scale-001'))
        .fetch<StartOrderRes>(),
      apiB.post('/orders/start')
        .body(startOrderReq('cart-success', 'addr-office', 'pm-ok', 'order-scale-002'))
        .fetch<StartOrderRes>()
    ]);
    const [scaleAConfirmed, scaleBConfirmed] = await Promise.all([
      this.waitForStatus(apiB, scaleA.state.orderId, OrderStatuses.Confirmed, signal),
      this.waitForStatus(apiA, scaleB.state.orderId, OrderStatuses.Confirmed, signal)
    ]);
    zlinkStreamAssert.ensure(scaleAConfirmed.status === OrderStatuses.Confirmed, 'Sample scenario assertion failed.');
    zlinkStreamAssert.ensure(scaleBConfirmed.status === OrderStatuses.Confirmed, 'Sample scenario assertion failed.');
    reportOrder('scale-a', scaleA.state.orderId);
    reportOrder('scale-b', scaleB.state.orderId);
  }

  private async assertContinued(api: ZLinkHttpClient, orderId: string, signal?: AbortSignal): Promise<void> {
    const continued = await api.post(`/orders/${encodeURIComponent(orderId)}/continue`)
      .fetch<ContinueOrderWorkflowRes>();
    zlinkStreamAssert.ensure(continued.state.status === OrderStatuses.Confirmed, 'Sample scenario assertion failed.');
    const confirmed = await this.waitForStatus(api, orderId, OrderStatuses.Confirmed, signal);
    zlinkStreamAssert.ensure(confirmed.reservationId === `reservation-${orderId}`, 'Sample scenario assertion failed.');
    zlinkStreamAssert.ensure(confirmed.paymentId === `payment-${orderId}`, 'Sample scenario assertion failed.');
  }

  private async getOrder(api: ZLinkHttpClient, orderId: string): Promise<OrderState> {
    return (await api.get(`/orders/${encodeURIComponent(orderId)}`).fetch<GetOrderStateRes>()).state;
  }

  private async startWithRetry(
    api: ZLinkHttpClient,
    request: StartOrderReq,
    signal?: AbortSignal
  ): Promise<StartOrderRes> {
    let lastFailure: unknown;
    for (let attempt = 0; attempt < 300; attempt += 1) {
      try {
        return await api.post('/orders/start').body(request).fetch<StartOrderRes>();
      } catch (error) {
        lastFailure = error;
      }
      await delay(100, signal);
    }
    throw lastFailure;
  }

  private async waitForStatus(
    api: ZLinkHttpClient,
    orderId: string,
    expectedStatus: string,
    signal?: AbortSignal
  ): Promise<OrderState> {
    let last: OrderState | undefined;
    for (let attempt = 0; attempt < 300; attempt += 1) {
      last = await this.getOrder(api, orderId);
      if (last.status === expectedStatus) return last;
      await delay(100, signal);
    }
    throw new Error(`Order '${orderId}' did not reach '${expectedStatus}' (last=${last?.status ?? 'none'}).`);
  }

  private isStartedOrConfirmed(state: OrderState): boolean {
    return state.status === OrderStatuses.Created ||
      state.status === OrderStatuses.InventoryReserved ||
      state.status === OrderStatuses.PaymentAuthorized ||
      state.status === OrderStatuses.Confirmed;
  }
}

function startOrderReq(
  cartId: string,
  shippingAddressId: string,
  paymentMethodId: string,
  idempotencyKey: string
): StartOrderReq {
  return { cartId, shippingAddressId, paymentMethodId, idempotencyKey };
}

function reportOrder(name: string, orderId: string): void {
  console.log(`shoppingmall-client-order name=${name} order=${orderId}`);
}

function delay(ms: number, signal?: AbortSignal): Promise<void> {
  return new Promise((resolve, reject) => {
    const timer = setTimeout(resolve, ms);
    signal?.addEventListener('abort', () => {
      clearTimeout(timer);
      reject(new DOMException('Operation aborted.', 'AbortError'));
    }, { once: true });
  });
}

export { ShoppingMallClientScenario };
