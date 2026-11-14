import { actorMessageFollowContext } from '../actors/actor-message-follow-context';
import type { ActorRef, RoutingId, ZLinkActor } from '../../contracts';
import type { ZLinkBackendActorRef } from '../backend/contracts';
import { routingIdsEqual } from '../routing-id';
import type { Message } from '../../contracts/Common/Message';
import type {
  ZLinkActorPacketDelivery,
  ZLinkActorRequestTerminal
} from './spot-actor-packet-dispatch';
import type { ZLinkRemoteBoundSessionTarget } from '../actors';
import type { ApplicationJobOrigin } from '../application-jobs/contracts';
import type { ZLinkSpotActorHandoffRuntime } from './spot-runtime-ports';
import {
  ZLinkFrameworkInternalErrorKind,
  createInternalFrameworkException
} from '../framework-errors-internal';

/** Parks an exact Actor incarnation while its accepted Join prepares the target. */
export type ZLinkRouteToActorJoinPrewarm = (
  actorId: string,
  objectGeneration: bigint,
  arrival: {
    readonly origin: ApplicationJobOrigin;
    readonly header: Buffer;
    readonly payload: Buffer;
    readonly returnResponse: boolean;
    readonly remoteBoundSessionTarget?: ZLinkRemoteBoundSessionTarget;
    readonly fallbackActorRef?: ActorRef;
    readonly requestTerminal?: ZLinkActorRequestTerminal;
    readonly resolve: (value: unknown) => void;
    readonly reject: (reason: unknown) => void;
  }
) => 'parked' | 'not-found';

export type ZLinkActorDispatchOwner =
  | {
      readonly nativeActorRef?: undefined;
      readonly meshName?: undefined;
      readonly spotId?: undefined;
    }
  | {
      readonly nativeActorRef: ZLinkBackendActorRef;
      readonly meshName: string;
      readonly spotId?: RoutingId;
    };

export interface ZLinkActorIngressMembership {
  readonly resolveOwner?: (actorId: string) => ZLinkActorDispatchOwner;
  readonly dispatchToOwner?: (
    spotId: RoutingId | undefined,
    delivery: ZLinkActorPacketDelivery
  ) => Promise<unknown>;
}

export type ZLinkActorIngressAdmission =
  | { readonly kind: 'local'; readonly actor: ZLinkActor }
  | { readonly kind: 'follow' | 'prewarm'; readonly terminal: Promise<unknown> }
  | { readonly kind: 'stale'; readonly error: ReturnType<typeof createInternalFrameworkException> };

/** Fixes one packet owner synchronously at the Actor FIFO head. */
export function resolveActorIngress(
  delivery: ZLinkActorPacketDelivery,
  request: boolean,
  options: {
    readonly handoffRuntime?: ZLinkSpotActorHandoffRuntime;
    readonly spotId?: RoutingId;
    readonly membership?: ZLinkActorIngressMembership;
    readonly resolveActor: (actorId: string) => ZLinkActor | undefined;
    readonly routeToActorJoinPrewarm?: ZLinkRouteToActorJoinPrewarm;
  }
): ZLinkActorIngressAdmission {
  const handoff = options.handoffRuntime?.capture(
    delivery.origin,
    delivery.actorId,
    delivery.parts,
    delivery.returnResponse,
    delivery.remoteBoundSessionTarget,
    delivery.fallbackActorRef,
    undefined,
    delivery.messageFollowOrigin
  );
  if (handoff !== undefined) return { kind: 'follow', terminal: handoff };
  const membership = options.membership;
  const owner = membership?.resolveOwner?.(delivery.actorId);
  if (owner?.nativeActorRef !== undefined && !routingIdsEqual(owner.spotId, options.spotId)) {
    if (membership?.dispatchToOwner === undefined) {
      throw new Error('Actor ingress requires the current membership dispatch port.');
    }
    // Transfer the accepted delivery before releasing the source FIFO claim.
    // Its terminal is awaited outside that claim by dispatchActorPacketInMailbox.
    return { kind: 'follow', terminal: membership.dispatchToOwner(owner.spotId, delivery) };
  }
  const actor =
    membership?.resolveOwner === undefined || owner?.nativeActorRef !== undefined
      ? options.resolveActor(delivery.actorId)
      : undefined;
  if (actor !== undefined) return { kind: 'local', actor };
  if (
    options.routeToActorJoinPrewarm !== undefined &&
    delivery.fallbackActorRef !== undefined &&
    (!request || delivery.returnResponse || delivery.requestTerminal !== undefined)
  ) {
    const parked = parkForActorJoinPrewarm(
      delivery.origin,
      options.routeToActorJoinPrewarm,
      delivery.actorId,
      delivery.parts,
      request,
      delivery.returnResponse,
      delivery.remoteBoundSessionTarget,
      delivery.fallbackActorRef,
      delivery.requestTerminal
    );
    if (parked !== undefined) return { kind: 'prewarm', terminal: parked };
  }
  return {
    kind: 'stale',
    error: createInternalFrameworkException(
      actorMessageFollowContext(delivery.fallbackActorRef) === undefined
        ? ZLinkFrameworkInternalErrorKind.ActorRouteNotFound
        : ZLinkFrameworkInternalErrorKind.ActorLocationStale,
      `SPOT actor is not registered locally: ${delivery.actorId}`
    )
  };
}

function parkForActorJoinPrewarm(
  origin: ApplicationJobOrigin,
  routeToActorJoinPrewarm: ZLinkRouteToActorJoinPrewarm,
  actorId: string,
  parts: readonly Message[],
  isRequest: boolean,
  returnResponse: boolean,
  remoteBoundSessionTarget: ZLinkRemoteBoundSessionTarget | undefined,
  fallbackActorRef: ActorRef,
  requestTerminal: ZLinkActorRequestTerminal | undefined
): Promise<unknown> | undefined {
  let resolve: (value: unknown) => void = () => undefined;
  let reject: (reason: unknown) => void = () => undefined;
  const result = new Promise<unknown>((res, rej) => {
    resolve = res;
    reject = rej;
  });
  //  A Request's real reply must still travel through the exact mailbox
  //  correlation the original caller captured in `requestTerminal` — not
  //  through this method's own return value, which the caller only uses
  //  to decide whether it still owes a reply itself. Wrapping keeps that
  //  reply route intact across the (possibly much later) redelivery at
  //  migration time while also releasing the original caller once the
  //  real reply has gone out, so it never sends a premature empty one.
  const wrappedRequestTerminal: ZLinkActorRequestTerminal | undefined =
    requestTerminal === undefined
      ? undefined
      : Object.assign(
          async (response: unknown, preparedReply?: unknown) => {
            try {
              await requestTerminal(response, preparedReply);
              resolve(undefined);
            } catch (error) {
              reject(error);
              throw error;
            }
          },
          { prepare: requestTerminal.prepare }
        );
  const route = routeToActorJoinPrewarm(actorId, fallbackActorRef.objectGeneration, {
    origin,
    header: Buffer.from(parts[0].data()),
    payload: Buffer.from(parts[1].data()),
    returnResponse,
    remoteBoundSessionTarget,
    fallbackActorRef,
    requestTerminal: wrappedRequestTerminal,
    resolve,
    reject
  });
  if (route !== 'parked') return undefined;
  //  A Send has no reply to wait for: the migrated redelivery still runs
  //  in order later, but this caller's own await is done once parking is
  //  confirmed.
  if (!isRequest) resolve(undefined);
  return result;
}
