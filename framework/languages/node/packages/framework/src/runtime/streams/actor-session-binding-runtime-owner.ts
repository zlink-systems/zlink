import type { RoutingId } from '../../contracts';
import type {
  ServiceSessionRelocationSeal,
  ServiceSessionRelocationRoute
} from '../foundation/service-stateful-wire-codec';
import type { ActorRef } from '../../contracts';
import type {
  ZLinkActorSessionAuthorityFence,
  ZLinkActorSessionRelocationSnapshot,
  ZLinkPreparedSessionRelocationSeal
} from './actor-session-binding-registry';

export interface ZLinkActorSessionBindingRuntimeOwner {
  prepareSessionRelocationSeal(
    value: ServiceSessionRelocationSeal,
    signal?: AbortSignal
  ): Promise<ZLinkPreparedSessionRelocationSeal | undefined>;
  acceptSessionRelocationRoute(
    value: ServiceSessionRelocationRoute,
    apply: () => Promise<void>
  ): Promise<boolean>;
  clearSessionRelocationResults(): Promise<void>;
  actorSlot(actorId: string, sessionRid: RoutingId): Promise<number | undefined>;
  relocationSnapshot(
    actorId: string,
    sealId: string
  ): Promise<ZLinkActorSessionRelocationSnapshot | undefined>;
  applyRelocation(
    actorId: string,
    sealId: string,
    applyFingerprint: string,
    commitOwnerTransition: () => Promise<void>
  ): Promise<void>;
  observeRelocationTerminal(
    actorId: string,
    sealId: string,
    applyFingerprint: string
  ): Promise<void>;
  clearRelocation(actorId: string, error: unknown, sealId?: string): Promise<void>;
  committedRoute(actorId: string): Promise<
    | {
        readonly actor: ActorRef;
        readonly authorityFence?: ZLinkActorSessionAuthorityFence;
      }
    | undefined
  >;
}

const owners = new WeakMap<object, ZLinkActorSessionBindingRuntimeOwner>();

export function registerActorSessionBindingRuntimeOwner(
  runtime: object,
  owner: ZLinkActorSessionBindingRuntimeOwner
): void {
  owners.set(runtime, owner);
}

export function actorSessionBindingRuntimeOwner(
  runtime: object
): ZLinkActorSessionBindingRuntimeOwner {
  const owner = owners.get(runtime);
  if (owner === undefined) {
    throw new Error('Stream binding runtime does not have a Session binding aggregate owner.');
  }
  return owner;
}

export function actorSessionBindingRuntimeOwnerIfRegistered(
  runtime: object
): ZLinkActorSessionBindingRuntimeOwner | undefined {
  return owners.get(runtime);
}
