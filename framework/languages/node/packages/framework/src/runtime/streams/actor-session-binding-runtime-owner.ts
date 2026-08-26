import type { ActorRef } from '../../contracts';
import type {
  ServiceSessionBindingAdmissionClaim,
  ServiceSessionBindingAdmissionResult
} from '../foundation/service-session-binding-ingress-port';
import type {
  ZLinkActorSessionAuthorityFence,
  ZLinkActorSessionAcceptedProducerProof,
  ZLinkActorSessionRelocationClaim,
  ZLinkActorSessionRelocationSnapshot,
  ZLinkActorSessionRetainedOutbound,
  ZLinkActorSessionRouteFence
} from './actor-session-binding-registry';

export interface ZLinkActorSessionBindingRuntimeOwner {
  sealRelocation(
    claim: ZLinkActorSessionRelocationClaim,
    expected: ZLinkActorSessionRouteFence,
    signal?: AbortSignal
  ): Promise<void>;
  relocationSnapshot(
    actorId: string,
    sealId: string
  ): Promise<ZLinkActorSessionRelocationSnapshot | undefined>;
  retainRelocationOutbound(
    actorId: string,
    operation: ZLinkActorSessionRetainedOutbound,
    sealId?: string
  ): Promise<ServiceSessionBindingAdmissionResult>;
  admitRelocationOutbound(
    claim: ServiceSessionBindingAdmissionClaim,
    operation: ZLinkActorSessionRetainedOutbound
  ): Promise<ServiceSessionBindingAdmissionResult>;
  discardRelocationOutbound(actorId: string, sealId: string, error: unknown): Promise<void>;
  applyRelocation(
    actorId: string,
    sealId: string,
    applyFingerprint: string,
    action: 'commit' | 'abort',
    commitOwnerTransition: () => Promise<void>,
    acceptedProducerProof?: ZLinkActorSessionAcceptedProducerProof
  ): Promise<void>;
  observeRelocationTerminal(
    actorId: string,
    sealId: string,
    applyFingerprint: string
  ): Promise<void>;
  clearRelocation(actorId: string, error: unknown): Promise<void>;
  committedRoute(actorId: string): Promise<{
    readonly actor: ActorRef;
    readonly authorityFence?: ZLinkActorSessionAuthorityFence;
  } | undefined>;
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
