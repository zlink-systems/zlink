type DeliveryStatus = 'Created' | 'Assigned' | 'Accepted' | 'Reassigned' | 'PickedUp' | 'Delivered' | 'Failed';

type CreateDeliveryReq = {
  deliveryId: string;
  customerId: string;
  pickupAddress: string;
  dropoffAddress: string;
};

type CreateDeliveryRes = {
  deliveryId: string;
};

class SubscribeDeliveryReq { constructor(readonly deliveryId: string) {} }

class SubscribeDeliveryRes { constructor(readonly deliveryId: string) {} }

class BindCourierSessionReq { constructor(readonly courierId: string) {} }

type BindCourierSessionRes = { courierId: string };

class EnsureCourierActorReq { constructor(readonly courierId: string) {} }


class AssignDeliveryMsg {
  constructor(
    readonly deliveryId: string,
    readonly customerId: string,
    readonly pickupAddress: string,
    readonly dropoffAddress: string
  ) {}
}

class OfferDeliveryMsg {
  constructor(
    readonly deliveryId: string,
    readonly courierId: string,
    readonly attempt: number,
    readonly pickupAddress: string,
    readonly dropoffAddress: string
  ) {}
}

class OfferDeliveryResultMsg {
  constructor(
    readonly deliveryId: string,
    readonly courierId: string,
    readonly attempt: number,
    readonly accepted: boolean,
    readonly reason?: string
  ) {}
}

class OfferDeliveryNotify {
  constructor(
    readonly courierId: string,
    readonly deliveryId: string,
    readonly pickupAddress: string,
    readonly dropoffAddress: string
  ) {}
}

class CourierDecisionMsg {
  constructor(
    readonly deliveryId: string,
    readonly courierId: string,
    readonly accepted: boolean,
    readonly reason?: string
  ) {}
}

class DeliveryStatusChangedReq {
  constructor(
    readonly deliveryId: string,
    readonly customerId: string,
    readonly status: Exclude<DeliveryStatus, 'Created'>,
    readonly occurredAtUnixMs: number,
    readonly courierId?: string
  ) {}
}

type DeliveryStatusChangedRes = {
  deliveryId: string;
  status: DeliveryStatus;
};

class DeliveryStatusNotify {
  constructor(
    readonly deliveryId: string,
    readonly status: DeliveryStatus,
    readonly occurredAtUnixMs: number,
    readonly courierId?: string
  ) {}
}

class DeliveryStatusUpdatedMsg {
  constructor(
    readonly deliveryId: string,
    readonly customerId: string,
    readonly status: Exclude<DeliveryStatus, 'Created'>,
    readonly occurredAtUnixMs: number,
    readonly courierId?: string
  ) {}
}

type ServerAssertionReq = {
  successfulDeliveryId: string;
  reassignedDeliveryId: string;
};

type ServerAssertionRes = {
  passed: boolean;
  evidence: string[];
};

const PacketNames = {
  assignDelivery: 'AssignDeliveryMsg',
  bindCourierSession: 'BindCourierSessionReq',
  bindCourierSessionResult: 'BindCourierSessionRes',
  createDeliveryRequest: 'CreateDeliveryReq',
  deliveryCreated: 'CreateDeliveryRes',
  deliveryStatusAck: 'DeliveryStatusChangedRes',
  deliveryStatusChanged: 'DeliveryStatusChangedReq',
  deliveryStatusNotify: 'DeliveryStatusNotify',
  deliveryStatusUpdated: 'DeliveryStatusUpdatedMsg',
  findCourierActor: 'FindCourierActorReq',
  courierActorFound: 'FindCourierActorRes',
  ensureCourierActor: 'EnsureCourierActorReq',
  courierActorEnsured: 'EnsureCourierActorRes',
  findCustomerActor: 'FindCustomerActorReq',
  customerActorFound: 'FindCustomerActorRes',
  ensureCustomerActor: 'EnsureCustomerActorReq',
  customerActorEnsured: 'EnsureCustomerActorRes',
  offerDelivery: 'OfferDeliveryMsg',
  offerDeliveryNotify: 'OfferDeliveryNotify',
  offerDeliveryResult: 'OfferDeliveryResultMsg',
  courierDecision: 'CourierDecisionMsg',
  serverAssertionReq: 'ServerAssertionReq',
  serverAssertionRes: 'ServerAssertionRes',
  subscribeDelivery: 'SubscribeDeliveryReq',
  subscribeDeliveryAccepted: 'SubscribeDeliveryRes'
} as const;

function assignDelivery(
  deliveryId: string,
  customerId: string,
  pickupAddress: string,
  dropoffAddress: string
): AssignDeliveryMsg {
  return new AssignDeliveryMsg(deliveryId, customerId, pickupAddress, dropoffAddress);
}

function bindCourierSession(courierId: string): BindCourierSessionReq {
  return new BindCourierSessionReq(courierId);
}

function deliveryStatusChanged(
  deliveryId: string,
  customerId: string,
  status: DeliveryStatusChangedReq['status'],
  courierId?: string
): DeliveryStatusChangedReq {
  return new DeliveryStatusChangedReq(deliveryId, customerId, status, Date.now(), courierId);
}

function ensureCourierActor(courierId: string): EnsureCourierActorReq {
  return new EnsureCourierActorReq(courierId);
}

function offerDelivery(
  courierId: string,
  deliveryId: string,
  attempt: number,
  pickupAddress: string,
  dropoffAddress: string
): OfferDeliveryMsg {
  return new OfferDeliveryMsg(deliveryId, courierId, attempt, pickupAddress, dropoffAddress);
}

function subscribeDelivery(deliveryId: string): SubscribeDeliveryReq {
  return new SubscribeDeliveryReq(deliveryId);
}

export {
  DeliveryStatusNotify,
  OfferDeliveryNotify,
  DeliveryStatusUpdatedMsg,
  SubscribeDeliveryReq,
  SubscribeDeliveryRes,
  BindCourierSessionReq,
  EnsureCourierActorReq,
  AssignDeliveryMsg,
  OfferDeliveryMsg,
  OfferDeliveryResultMsg,
  DeliveryStatusChangedReq,
  CourierDecisionMsg,
  PacketNames,
  assignDelivery,
  bindCourierSession,
  deliveryStatusChanged,
  ensureCourierActor,
  offerDelivery,
  subscribeDelivery
};

export type {
  BindCourierSessionRes,
  CreateDeliveryReq,
  CreateDeliveryRes,
  DeliveryStatus,
  DeliveryStatusChangedRes,
  ServerAssertionReq,
  ServerAssertionRes
};
