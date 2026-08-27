import { CourierDecisionMsg, bindCourierSession, PacketNames, subscribeDelivery } from '../Shared/Contracts/messages';
import type { BrowserHttpClient } from '../../browser-client-runtime';
import { zlinkStreamAssert } from '@zlink-systems/stream-connector';
import type { ZlinkStreamConnector } from '@zlink-systems/stream-connector';
import type {
  CreateDeliveryRes,
  BindCourierSessionRes,
  DeliveryStatusNotify,
  OfferDeliveryNotify,
  ServerAssertionRes,
  SubscribeDeliveryRes
} from '../Shared/Contracts/messages';

class DeliveryDispatchClientScenario {
  async run(
    http: BrowserHttpClient,
    createCustomer: () => ZlinkStreamConnector,
    createCourierA: () => ZlinkStreamConnector,
    createCourierB: () => ZlinkStreamConnector,
    signal?: AbortSignal
  ): Promise<void> {
    const customer = createCustomer();
    const courierA = createCourierA();
    const courierB = createCourierB();
    try {
      await this.connect(customer, courierA, courierB, signal);
      await this.bindCourier(courierA, 'courier-a', signal);
      await this.bindCourier(courierB, 'courier-b', signal);
      await this.runSuccessfulDelivery(http, customer, courierA, signal);
      await this.runReassignedDelivery(http, customer, courierA, courierB, signal);
      await this.runExhaustedDelivery(http, customer, courierA, courierB, signal);
      await this.assertServerEvidence(http);
    } finally {
      await Promise.allSettled([customer.close(), courierA.close(), courierB.close()]);
    }
  }

  private async connect(
    customer: ZlinkStreamConnector,
    courierA: ZlinkStreamConnector,
    courierB: ZlinkStreamConnector,
    signal?: AbortSignal
  ): Promise<void> {
    await Promise.all([customer.connect(signal), courierA.connect(signal), courierB.connect(signal)]);
  }

  private async bindCourier(
    courier: ZlinkStreamConnector,
    courierId: string,
    signal?: AbortSignal
  ): Promise<BindCourierSessionRes> {
    const bound = await courier.request(bindCourierSession(courierId), Object)
      .packetName(PacketNames.bindCourierSession)
      .submit<BindCourierSessionRes>(signal);
    zlinkStreamAssert.ensure(bound.courierId === courierId, 'Sample scenario assertion failed.');
    return bound;
  }

  private async runSuccessfulDelivery(
    http: BrowserHttpClient,
    customer: ZlinkStreamConnector,
    courierA: ZlinkStreamConnector,
    signal?: AbortSignal
  ): Promise<void> {
    const deliveryId = 'delivery-success';
    const offerA = courierA.waitFor<OfferDeliveryNotify>(PacketNames.offerDeliveryNotify)
      .where((message) => message.payload.deliveryId === deliveryId)
      .submit(signal);
    const statuses = ['Assigned', 'Accepted', 'PickedUp', 'Delivered'] as const;
    const statusSequence = statuses.reduce(
      (sequence, status) => sequence.expect((payload) =>
        payload.deliveryId === deliveryId && payload.status === status),
      customer.waitForSequence<DeliveryStatusNotify>(PacketNames.deliveryStatusNotify)
    ).run(signal);

    const subscribed = await customer.request(subscribeDelivery(deliveryId), Object)
      .packetName(PacketNames.subscribeDelivery)
      .submit<SubscribeDeliveryRes>(signal);
    zlinkStreamAssert.ensure(subscribed.deliveryId === deliveryId, 'Sample scenario assertion failed.');

    const created = await http.post('/deliveries')
      .body({
        deliveryId,
        customerId: 'customer-1',
        pickupAddress: 'Kitchen 12',
        dropoffAddress: 'Customer Lobby'
      })
      .fetch<CreateDeliveryRes>();
    zlinkStreamAssert.ensure(created.deliveryId === deliveryId, 'Sample scenario assertion failed.');
    const offered = await offerA;
    zlinkStreamAssert.ensure(offered.payload.courierId === 'courier-a', 'Sample scenario assertion failed.');
    await courierA.send(new CourierDecisionMsg(deliveryId, 'courier-a', true))
      .packetName(PacketNames.courierDecision)
      .submit();
    const notifications = await statusSequence;
    zlinkStreamAssert.ensure(notifications.every((payload) => payload.courierId === 'courier-a'), 'Sample scenario assertion failed.');
  }

  private async runReassignedDelivery(
    http: BrowserHttpClient,
    customer: ZlinkStreamConnector,
    courierA: ZlinkStreamConnector,
    courierB: ZlinkStreamConnector,
    signal?: AbortSignal
  ): Promise<void> {
    const deliveryId = 'delivery-reassign';
    const timedOutOffer = courierA.waitFor<OfferDeliveryNotify>(PacketNames.offerDeliveryNotify)
      .where((message) => message.payload.deliveryId === deliveryId)
      .submit(signal);
    const reassignedOffer = courierB.waitFor<OfferDeliveryNotify>(PacketNames.offerDeliveryNotify)
      .where((message) => message.payload.deliveryId === deliveryId)
      .submit(signal);
    const statuses = ['Assigned', 'Reassigned', 'Accepted', 'PickedUp', 'Delivered'] as const;
    const statusSequence = statuses.reduce(
      (sequence, status) => sequence.expect((payload) =>
        payload.deliveryId === deliveryId && payload.status === status),
      customer.waitForSequence<DeliveryStatusNotify>(PacketNames.deliveryStatusNotify)
    ).run(signal);

    const subscribed = await customer.request(subscribeDelivery(deliveryId), Object)
      .packetName(PacketNames.subscribeDelivery)
      .submit<SubscribeDeliveryRes>(signal);
    zlinkStreamAssert.ensure(subscribed.deliveryId === deliveryId, 'Sample scenario assertion failed.');

    const created = await http.post('/deliveries')
      .body({
        deliveryId,
        customerId: 'customer-1',
        pickupAddress: 'Kitchen 12',
        dropoffAddress: 'Customer Lobby'
      })
      .fetch<CreateDeliveryRes>();
    zlinkStreamAssert.ensure(created.deliveryId === deliveryId, 'Sample scenario assertion failed.');
    const firstOffer = await timedOutOffer;
    zlinkStreamAssert.ensure(firstOffer.payload.courierId === 'courier-a', 'Sample scenario assertion failed.');

    const secondOffer = await reassignedOffer;
    zlinkStreamAssert.ensure(secondOffer.payload.courierId === 'courier-b', 'Sample scenario assertion failed.');
    await courierB.send(new CourierDecisionMsg(deliveryId, 'courier-b', true))
      .packetName(PacketNames.courierDecision)
      .submit();

    const notifications = await statusSequence;
    zlinkStreamAssert.ensure(notifications[0]?.courierId === 'courier-a', 'Sample scenario assertion failed.');
    zlinkStreamAssert.ensure(notifications.slice(1).every((payload) => payload.courierId === 'courier-b'), 'Sample scenario assertion failed.');
    await courierA.send(new CourierDecisionMsg(deliveryId, 'courier-a', true))
      .packetName(PacketNames.courierDecision)
      .submit();
    console.log('deliverydispatch-reassignment=completed');
  }

  private async runExhaustedDelivery(
    http: BrowserHttpClient,
    customer: ZlinkStreamConnector,
    courierA: ZlinkStreamConnector,
    courierB: ZlinkStreamConnector,
    signal?: AbortSignal
  ): Promise<void> {
    const deliveryId = 'delivery-exhausted';
    const firstOffer = courierA.waitFor<OfferDeliveryNotify>(PacketNames.offerDeliveryNotify)
      .where((message) => message.payload.deliveryId === deliveryId)
      .submit(signal);
    const secondOffer = courierB.waitFor<OfferDeliveryNotify>(PacketNames.offerDeliveryNotify)
      .where((message) => message.payload.deliveryId === deliveryId)
      .submit(signal);
    const statusSequence = ['Assigned', 'Reassigned', 'Failed'].reduce(
      (sequence, status) => sequence.expect((payload) =>
        payload.deliveryId === deliveryId && payload.status === status),
      customer.waitForSequence<DeliveryStatusNotify>(PacketNames.deliveryStatusNotify)
    ).run(signal);

    const subscribed = await customer.request(subscribeDelivery(deliveryId), Object)
      .packetName(PacketNames.subscribeDelivery)
      .submit<SubscribeDeliveryRes>(signal);
    zlinkStreamAssert.ensure(subscribed.deliveryId === deliveryId, 'Sample scenario assertion failed.');

    const created = await http.post('/deliveries')
      .body({
        deliveryId,
        customerId: 'customer-1',
        pickupAddress: 'Kitchen 12',
        dropoffAddress: 'Customer Lobby'
      })
      .fetch<CreateDeliveryRes>();
    zlinkStreamAssert.ensure(created.deliveryId === deliveryId, 'Sample scenario assertion failed.');
    const offeredA = await firstOffer;
    zlinkStreamAssert.ensure(offeredA.payload.courierId === 'courier-a', 'Sample scenario assertion failed.');
    await courierA.send(new CourierDecisionMsg(deliveryId, 'courier-a', false, 'unavailable'))
      .packetName(PacketNames.courierDecision)
      .submit();
    const offeredB = await secondOffer;
    zlinkStreamAssert.ensure(offeredB.payload.courierId === 'courier-b', 'Sample scenario assertion failed.');
    await courierB.send(new CourierDecisionMsg(deliveryId, 'courier-b', false, 'unavailable'))
      .packetName(PacketNames.courierDecision)
      .submit();
    const notifications = await statusSequence;
    zlinkStreamAssert.ensure(notifications.filter((payload) => payload.status === 'Failed').length === 1, 'Sample scenario assertion failed.');
  }

  private async assertServerEvidence(http: BrowserHttpClient): Promise<void> {
    const assertion = await http.post('/self-check/assert')
      .body({
        successfulDeliveryId: 'delivery-success',
        reassignedDeliveryId: 'delivery-reassign'
      })
      .fetch<ServerAssertionRes>();
    zlinkStreamAssert.ensure(assertion.passed, 'Sample scenario assertion failed.');
    console.log('deliverydispatch-server-evidence=completed');
  }
}

export {
  DeliveryDispatchClientScenario
};
