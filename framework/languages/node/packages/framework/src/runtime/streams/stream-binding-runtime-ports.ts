import type { ActorRef, ZLinkSessionActor } from '../../contracts';
import type { ZLinkBackendActorSessionNode } from '../backend';
import type { ZLinkSubmitResult } from '../messaging/submission-result';
import type { DefaultZLinkSessionActor } from './session-context';
import type { ZLinkBoundSessionResponseTarget } from './bound-session-response-target';

export interface ZLinkStreamActorLookupPort {
  find(actorId: string): DefaultZLinkSessionActor | undefined;
  authorityFence(actorId: string): {
    readonly authorityOwnerGeneration: bigint;
    readonly ownerLeaseGeneration: bigint;
  } | undefined;
  sessionRouteFence(actorId: string): {
    readonly actor: ActorRef;
    readonly sessionRid: ActorRef['nodeRid'];
    readonly bindingGeneration: bigint;
  } | undefined;
}

export interface ZLinkStreamActorLifecyclePort {
  rebindActor(actorRef: ActorRef, signal?: AbortSignal): Promise<void>;
  refreshActor(actorRef: ActorRef, signal?: AbortSignal): Promise<void>;
  commitActorRoute(
    actorRef: ActorRef,
    signal?: AbortSignal,
    options?: ZLinkActorRouteCommitOptions
  ): Promise<void>;
  abortActorRouteSeal(actorId: string, sealId: string): boolean;
  validateActorRouteSeal(actorId: string, sealId: string): boolean;
  unbindActor(actorId: string): void;
}

export interface ZLinkActorRouteCommitOptions {
  /**
   * Relocation already carries the binding identity to the target actor. The
   * follow-up confirmation can use a one-way route submission because a
   * request/ACK would create a nested route request while the Session
   * ownership command is being dispatched.
   */
  readonly confirmRemoteSessionBinding?: boolean | 'send';
  /**
   * Command 44 keeps ingress sealed while the native route is prepared, then
   * publishes the new route and releases this exact seal in one owner turn.
   */
  readonly releaseSeal?: {
    readonly sealId: string;
  };
}

export interface ZLinkBoundSessionResponsePort {
  captureBoundSessionResponseTarget(actor: ZLinkSessionActor): ZLinkBoundSessionResponseTarget | undefined;
  sendLocalBoundSessionResponse(
    actorId: string, packetName: string, requestSeq: bigint, message: unknown,
    metadata: ReadonlyMap<string, string>, compressPayload: boolean
  ): boolean;
  sendLocalBoundSessionError(
    actorId: string, packetName: string, requestSeq: bigint, error: unknown,
    metadata: ReadonlyMap<string, string>
  ): boolean;
}

export interface ZLinkRemoteBoundSessionPort extends ZLinkStreamActorLifecyclePort {
  disconnectBoundSession(actorId: string, signal?: AbortSignal): Promise<void>;
  sendLocalBoundSession(
    actorId: string, message: unknown, packetName: string | undefined,
    metadata: ReadonlyMap<string, string>
  ): boolean;
  sendLocalBoundSessionResponse(
    actorId: string, packetName: string, requestSeq: bigint, message: unknown,
    metadata: ReadonlyMap<string, string>, compressPayload: boolean
  ): boolean;
  sendLocalBoundSessionError(
    actorId: string, packetName: string, requestSeq: bigint, error: unknown,
    metadata: ReadonlyMap<string, string>
  ): boolean;
  sendNativeBoundSessionResponse(
    node: ZLinkBackendActorSessionNode, actorRef: ActorRef, packetName: string, requestSeq: bigint,
    message: unknown, metadata: ReadonlyMap<string, string>, compressPayload: boolean,
    signal?: AbortSignal
  ): Promise<void>;
  sendNativeBoundSessionError(
    node: ZLinkBackendActorSessionNode, actorRef: ActorRef, packetName: string, requestSeq: bigint,
    error: unknown, metadata: ReadonlyMap<string, string>, signal?: AbortSignal
  ): Promise<void>;
}

export interface ZLinkNativeFallbackBoundSessionPort {
  disconnectNativeBoundSession(node: ZLinkBackendActorSessionNode, actorRef: ActorRef, signal?: AbortSignal): Promise<void>;
  disconnectBoundSession(actorId: string, signal?: AbortSignal): Promise<void>;
  sendLocalBoundSession(
    actorId: string, message: unknown, packetName: string | undefined,
    metadata: ReadonlyMap<string, string>
  ): boolean;
  submitLocalBoundSession(
    actorId: string, message: unknown, packetName: string | undefined,
    metadata: ReadonlyMap<string, string>, signal?: AbortSignal
  ): Promise<ZLinkSubmitResult>;
  sendNativeBoundSession(
    node: ZLinkBackendActorSessionNode, actorRef: ActorRef, message: unknown, packetName: string | undefined,
    metadata: ReadonlyMap<string, string>, signal?: AbortSignal
  ): Promise<ZLinkSubmitResult>;
  sendBoundSession(
    actorId: string, message: unknown, packetName: string | undefined,
    metadata: ReadonlyMap<string, string>, signal?: AbortSignal
  ): Promise<ZLinkSubmitResult>;
}
