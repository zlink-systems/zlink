import type { ZLinkActorMessageFollowContext } from './actor-message-follow-context';

export const ZLINK_INTERNAL_ACTOR_TRANSPORT_DELIVERY_GATE =
  Symbol.for('zlink.internal.actor.transport-delivery-gate');

export interface ZLinkInternalActorTransportDeliveryGate {
  waitBeforeSubmit(
    actorId: string,
    kind: 'oneWay' | 'request',
    signal?: AbortSignal
  ): Promise<number | void>;

  recordMessageFollowRelay?(
    actorId: string,
    context: ZLinkActorMessageFollowContext
  ): void;
}
