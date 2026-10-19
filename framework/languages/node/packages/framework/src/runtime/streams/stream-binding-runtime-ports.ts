import type { ActorRef, ZLinkSessionActor } from '../../contracts';
import type { ZLinkBackendActorSessionNode } from '../backend';
import type { ZLinkSubmitResult } from '../messaging/submission-result';
import type { DefaultZLinkSessionActor } from './session-context';
import type { ZLinkBoundSessionResponseTarget } from './bound-session-response-target';
import type { ServiceStreamSessionBinding } from '../foundation/service-runtime-contracts';

export interface ZLinkStreamActorLookupPort {
  find(actorId: string): Promise<DefaultZLinkSessionActor | undefined>;
  authorityFence(actorId: string): Promise<
    | {
        readonly authorityOwnerGeneration: bigint;
        readonly ownerLeaseGeneration: bigint;
      }
    | undefined
  >;
  sessionRouteFence(actorId: string): Promise<
    | {
        readonly actor: ActorRef;
        readonly sessionRid: ActorRef['nodeRid'];
        readonly bindingGeneration: bigint;
      }
    | undefined
  >;
}

export interface ZLinkStreamActorLifecyclePort {
  rebindActor(actorRef: ActorRef, signal?: AbortSignal): Promise<void>;
  refreshActor(actorRef: ActorRef, signal?: AbortSignal): Promise<void>;
  commitActorRoute(
    actorRef: ActorRef,
    signal?: AbortSignal,
    options?: ZLinkActorRouteCommitOptions
  ): Promise<void>;
  abortActorRouteSeal(actorId: string, sealId: string): Promise<boolean>;
  validateActorRouteSeal(actorId: string, sealId: string): Promise<boolean>;
  unbindActor(actorId: string): Promise<void>;
}

export interface ZLinkActorRouteCommitOptions {
  /**
   * Command 44 publishes the Actor route of the existing binding and releases
   * this exact seal in one owner turn.
   */
  readonly releaseSeal?: {
    readonly sealId: string;
  };
}

export interface ZLinkBoundSessionResponsePort {
  captureBoundSessionResponseTarget(
    actor: ZLinkSessionActor
  ): Promise<ZLinkBoundSessionResponseTarget | undefined>;
  sendLocalBoundSessionResponse(
    actorId: string,
    packetName: string,
    requestSeq: bigint,
    message: unknown,
    metadata: ReadonlyMap<string, string>,
    compressPayload: boolean
  ): Promise<boolean>;
  sendLocalBoundSessionError(
    actorId: string,
    packetName: string,
    requestSeq: bigint,
    error: unknown,
    metadata: ReadonlyMap<string, string>
  ): Promise<boolean>;
}

/**
 * Session–Actor binding §5: the Session owner's registry snapshot travels with
 * the bind confirmation so a same-node Actor owner installs the same identity a
 * remote command 38 would carry.
 */
export interface ZLinkSessionBindingConfirmationOptions {
  readonly waitForAcknowledgement?: boolean;
  readonly binding?: ServiceStreamSessionBinding;
}

/** The exact Session binding identity a relocation validated (§8.1). */
export interface ZLinkSessionBindingIdentity {
  readonly sessionRid: string;
  readonly bindingGeneration: bigint;
}

export interface ZLinkRemoteBoundSessionPort extends ZLinkStreamActorLifecyclePort {
  closeSessionBinding(
    actorId: string,
    expected: ZLinkSessionBindingIdentity,
    signal?: AbortSignal
  ): Promise<void>;
  sendLocalBoundSession(
    actorId: string,
    message: unknown,
    packetName: string | undefined,
    metadata: ReadonlyMap<string, string>
  ): Promise<boolean>;
  sendLocalBoundSessionResponse(
    actorId: string,
    packetName: string,
    requestSeq: bigint,
    message: unknown,
    metadata: ReadonlyMap<string, string>,
    compressPayload: boolean
  ): Promise<boolean>;
  sendLocalBoundSessionError(
    actorId: string,
    packetName: string,
    requestSeq: bigint,
    error: unknown,
    metadata: ReadonlyMap<string, string>
  ): Promise<boolean>;
  sendNativeBoundSessionResponse(
    node: ZLinkBackendActorSessionNode,
    actorRef: ActorRef,
    packetName: string,
    requestSeq: bigint,
    message: unknown,
    metadata: ReadonlyMap<string, string>,
    compressPayload: boolean,
    signal?: AbortSignal
  ): Promise<void>;
  sendNativeBoundSessionError(
    node: ZLinkBackendActorSessionNode,
    actorRef: ActorRef,
    packetName: string,
    requestSeq: bigint,
    error: unknown,
    metadata: ReadonlyMap<string, string>,
    signal?: AbortSignal
  ): Promise<void>;
}

export interface ZLinkNativeFallbackBoundSessionPort {
  disconnectNativeBoundSession(
    node: ZLinkBackendActorSessionNode,
    actorRef: ActorRef,
    timeoutMs: number,
    signal?: AbortSignal
  ): Promise<void>;
  disconnectBoundSession(actorId: string, signal?: AbortSignal): Promise<void>;
  sendLocalBoundSession(
    actorId: string,
    message: unknown,
    packetName: string | undefined,
    metadata: ReadonlyMap<string, string>
  ): Promise<boolean>;
  submitLocalBoundSession(
    actorId: string,
    message: unknown,
    packetName: string | undefined,
    metadata: ReadonlyMap<string, string>,
    signal?: AbortSignal
  ): Promise<ZLinkSubmitResult>;
  sendNativeBoundSession(
    node: ZLinkBackendActorSessionNode,
    actorRef: ActorRef,
    message: unknown,
    packetName: string | undefined,
    metadata: ReadonlyMap<string, string>,
    signal?: AbortSignal
  ): Promise<ZLinkSubmitResult>;
  sendBoundSession(
    actorId: string,
    message: unknown,
    packetName: string | undefined,
    metadata: ReadonlyMap<string, string>,
    signal?: AbortSignal
  ): Promise<ZLinkSubmitResult>;
}
