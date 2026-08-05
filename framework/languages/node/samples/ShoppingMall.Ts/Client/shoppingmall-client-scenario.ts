import { DecimalAmount, OrderStatuses } from '../Shared/Contracts/messages';
import type { ZLinkHttpClient } from '@zlink-systems/http-client';
import { zlinkStreamAssert } from '@zlink-systems/stream-connector';
import type {
  ContinueOrderWorkflowRes,
  GetOrderStateRes,
  OrderState,
  RebuildOrderProjectionRes,
  ServerAssertionRes,
  StartOrderReq,
  StartOrderRes
} from '../Shared/Contracts/messages';

type VersionFenceResult = {
  rejected: boolean;
  expectedVersion: number;
  actualVersion: number;
};

class ShoppingMallClientScenario {
  async run(apiA: ZLinkHttpClient, apiB: ZLinkHttpClient, signal?: AbortSignal): Promise<void> {
    const seeded = await apiA.post('/self-check/seed').submitRaw();
    zlinkStreamAssert.ensure(seeded.status >= 200 && seeded.status < 300, 'Sample scenario assertion failed.');

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
    console.log('shoppingmall-success=completed');

    const duplicate = await apiB.post('/orders/start').body(successReq).fetch<StartOrderRes>();
    zlinkStreamAssert.ensure(duplicate.state.orderId === success.state.orderId, 'Sample scenario assertion failed.');
    console.log('shoppingmall-idempotency=completed');

    const concurrentReq = startOrderReq('cart-success', 'addr-office', 'pm-ok', 'order-concurrent-001');
    const [concurrentA, concurrentB] = await Promise.all([
      apiA.post('/orders/start').body(concurrentReq).fetch<StartOrderRes>(),
      apiB.post('/orders/start').body(concurrentReq).fetch<StartOrderRes>()
    ]);
    zlinkStreamAssert.ensure(concurrentA.state.orderId === concurrentB.state.orderId, 'Sample scenario assertion failed.');
    const concurrentConfirmed = await this.waitForStatus(apiA, concurrentA.state.orderId, OrderStatuses.Confirmed, signal);
    zlinkStreamAssert.ensure(concurrentConfirmed.status === OrderStatuses.Confirmed, 'Sample scenario assertion failed.');
    console.log('shoppingmall-concurrent=completed');

    const pendingReq = startOrderReq('cart-success', 'addr-office', 'pm-ok', 'order-pending-001');
    const pendingHook = await apiA.post('/self-check/idempotency/pending')
      .body({ idempotencyKey: pendingReq.idempotencyKey, orderId: 'order-pending-0001', ownerInstanceId: 'api-a' })
      .submitRaw();
    zlinkStreamAssert.ensure(pendingHook.status >= 200 && pendingHook.status < 300, 'Sample scenario assertion failed.');
    const pending = await apiB.post('/orders/start').body(pendingReq).fetch<StartOrderRes>();
    zlinkStreamAssert.ensure(pending.state.orderId === 'order-pending-0001', 'Sample scenario assertion failed.');
    zlinkStreamAssert.ensure(pending.state.status === OrderStatuses.Created, 'Sample scenario assertion failed.');
    const pendingCreated = await this.getOrder(apiA, pending.state.orderId);
    zlinkStreamAssert.ensure(this.isStartedOrConfirmed(pendingCreated), 'Sample scenario assertion failed.');
    zlinkStreamAssert.ensure(pendingCreated.shippingAddressId === pendingReq.shippingAddressId, 'Sample scenario assertion failed.');
    console.log('shoppingmall-pending=completed');

    const resumeReq = startOrderReq('cart-success', 'addr-home', 'pm-ok', 'order-resume-001');
    const inventoryReserved = await apiA.post('/self-check/workflow/inventory-reserved')
      .body(resumeReq)
      .fetch<StartOrderRes>();
    zlinkStreamAssert.ensure(inventoryReserved.state.status === OrderStatuses.InventoryReserved, 'Sample scenario assertion failed.');
    const resumed = await apiB.post(`/self-check/workflow/${inventoryReserved.state.orderId}/continue`)
      .fetch<ContinueOrderWorkflowRes>();
    zlinkStreamAssert.ensure(resumed.state.status === OrderStatuses.Confirmed, 'Sample scenario assertion failed.');
    zlinkStreamAssert.ensure(resumed.state.reservationId === `reservation-${inventoryReserved.state.orderId}`, 'Sample scenario assertion failed.');
    zlinkStreamAssert.ensure(resumed.state.paymentId === `payment-${inventoryReserved.state.orderId}`, 'Sample scenario assertion failed.');
    console.log('shoppingmall-resume=completed');

    const interruptedReq = startOrderReq('cart-success', 'addr-home', 'pm-ok', 'order-interrupted-001');
    const interrupted = await apiA.post('/self-check/workflow/inventory-effect')
      .body(interruptedReq)
      .fetch<StartOrderRes>();
    zlinkStreamAssert.ensure(interrupted.state.status === OrderStatuses.Created, 'Sample scenario assertion failed.');
    const interruptionRecovered = await apiB.post(`/self-check/workflow/${interrupted.state.orderId}/continue`)
      .fetch<ContinueOrderWorkflowRes>();
    zlinkStreamAssert.ensure(interruptionRecovered.state.status === OrderStatuses.Confirmed, 'Sample scenario assertion failed.');
    console.log('shoppingmall-effect-interruption=completed');

    const fence = await apiA.post(`/self-check/workflow/${success.state.orderId}/verify-fence`)
      .fetch<VersionFenceResult>();
    zlinkStreamAssert.ensure(fence.rejected, 'Sample scenario assertion failed.');
    zlinkStreamAssert.ensure(fence.actualVersion > fence.expectedVersion, 'Sample scenario assertion failed.');
    console.log('shoppingmall-version-fence=completed');

    const inventoryReq = startOrderReq('cart-inventory-fail', 'addr-home', 'pm-ok', 'order-inventory-001');
    const inventoryStarted = await apiA.post('/orders/start').body(inventoryReq).fetch<StartOrderRes>();
    const inventoryFailed = await this.waitForStatus(apiA, inventoryStarted.state.orderId, OrderStatuses.Failed, signal);
    zlinkStreamAssert.ensure(inventoryFailed.reason?.toLowerCase().includes('inventory') === true, 'Sample scenario assertion failed.');
    console.log('shoppingmall-inventory-failure=completed');

    const paymentReq = startOrderReq('cart-payment-fail', 'addr-home', 'pm-decline', 'order-payment-001');
    const paymentStarted = await apiB.post('/orders/start').body(paymentReq).fetch<StartOrderRes>();
    const paymentFailed = await this.waitForStatus(apiB, paymentStarted.state.orderId, OrderStatuses.Failed, signal);
    zlinkStreamAssert.ensure(paymentFailed.reservationId !== undefined, 'Sample scenario assertion failed.');
    zlinkStreamAssert.ensure(paymentFailed.reason?.toLowerCase().includes('payment') === true, 'Sample scenario assertion failed.');
    console.log('shoppingmall-payment-failure=completed');

    const deleteProjection = await apiA.post(`/self-check/projection/${success.state.orderId}/delete`).submitRaw();
    zlinkStreamAssert.ensure(deleteProjection.status >= 200 && deleteProjection.status < 300, 'Sample scenario assertion failed.');
    const healedByContinue = await apiB.post(`/self-check/workflow/${success.state.orderId}/continue`)
      .fetch<ContinueOrderWorkflowRes>();
    zlinkStreamAssert.ensure(healedByContinue.state.status === OrderStatuses.Confirmed, 'Sample scenario assertion failed.');

    const deleteProjectionAgain = await apiA.post(`/self-check/projection/${success.state.orderId}/delete`).submitRaw();
    zlinkStreamAssert.ensure(deleteProjectionAgain.status >= 200 && deleteProjectionAgain.status < 300, 'Sample scenario assertion failed.');
    const rebuilt = await apiA.post(`/self-check/projection/${success.state.orderId}/rebuild`)
      .fetch<RebuildOrderProjectionRes>();
    zlinkStreamAssert.ensure(rebuilt.state.status === OrderStatuses.Confirmed, 'Sample scenario assertion failed.');
    const rebuiltRead = await this.getOrder(apiB, success.state.orderId);
    zlinkStreamAssert.ensure(rebuiltRead.status === OrderStatuses.Confirmed, 'Sample scenario assertion failed.');
    console.log('shoppingmall-rebuild=completed');

    const delayedFirst = await this.getOrder(apiB, paymentStarted.state.orderId);
    const delayedSecond = await this.getOrder(apiA, paymentStarted.state.orderId);
    zlinkStreamAssert.ensure(delayedFirst.status === delayedSecond.status, 'Sample scenario assertion failed.');
    zlinkStreamAssert.ensure(delayedSecond.status === OrderStatuses.Failed, 'Sample scenario assertion failed.');
    console.log('shoppingmall-consistency=completed');

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
    console.log('shoppingmall-scaleout=completed');

    const assertion = await apiA.post('/self-check/assert')
      .body({
        successfulOrderId: success.state.orderId,
        pendingRecoveredOrderId: pending.state.orderId,
        concurrentOrderId: concurrentA.state.orderId,
        resumedOrderId: inventoryReserved.state.orderId,
        interruptedOrderId: interrupted.state.orderId,
        inventoryFailureOrderId: inventoryStarted.state.orderId,
        paymentFailureOrderId: paymentStarted.state.orderId,
        scaleOutOrderIds: [scaleA.state.orderId, scaleB.state.orderId]
      })
      .fetch<ServerAssertionRes>();
    zlinkStreamAssert.ensure(assertion.passed, 'Sample scenario assertion failed.');
    console.log('shoppingmall-server-evidence=completed');
  }

  private async getOrder(api: ZLinkHttpClient, orderId: string): Promise<OrderState> {
    return (await api.get(`/orders/${orderId}`).fetch<GetOrderStateRes>()).state;
  }

  private async waitForStatus(
    api: ZLinkHttpClient,
    orderId: string,
    expectedStatus: string,
    signal?: AbortSignal
  ): Promise<OrderState> {
    let last: OrderState | undefined;
    for (let i = 0; i < 80; i++) {
      last = await this.getOrder(api, orderId);
      if (last.status === expectedStatus) {
        return last;
      }
      await delay(50, signal);
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
