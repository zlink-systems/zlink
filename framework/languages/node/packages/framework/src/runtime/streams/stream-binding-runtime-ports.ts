import type { ActorRef, ZLinkSessionActor } from '../../contracts';
import type { ZLinkBackendActorSessionNode } from '../backend';
import type { ZLinkSubmitResult } from '../messaging/submission-result';
import type { DefaultZLinkSessionActor } from './session-context';
import type { ZLinkBoundSessionResponseTarget } from './bound-session-response-target';

export interface ZLinkStreamActorLookupPort {
  find(actorId: string): DefaultZLinkSessionActor | undefined;
}

export interface ZLinkStreamActorLifecyclePort {
  rebindActor(actorRef: ActorRef, signal?: AbortSignal): Promise<void>;
  refreshActor(actorRef: ActorRef, signal?: AbortSignal): Promise<void>;
  commitActorRoute(actorRef: ActorRef, signal?: AbortSignal): Promise<void>;
  sealActorRoute(input: {
    readonly actorId: string;
    readonly actorGeneration: bigint;
    readonly actorOwnershipGeneration: bigint;
    readonly bindingGeneration: bigint;
    readonly ownerLeaseGeneration: bigint;
    readonly sealId: string;
  }): bigint;
  abortActorRouteSeal(actorId: string, sealId: string): boolean;
  validateActorRouteSeal(actorId: string, sealId: string, acceptedHighWater: bigint): boolean;
  unbindActor(actorId: string): void;
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
