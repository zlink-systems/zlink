import { DEFAULT_REQUEST_TIMEOUT_MS } from '../../contracts/Configuration/Registration';
import {
  createInternalFrameworkException,
  ZLinkFrameworkInternalErrorKind
} from '../framework-errors-internal';
import type { ActorRef, ZLinkBoundSession, ZLinkBoundSessionSendCall } from '../../contracts';
import { ZLinkSpotKind } from '../../contracts';
import {
  requireOneWayCompletion,
  throwAlreadySubmitted,
  ZLinkSubmitStatus,
  type ZLinkSubmitResult
} from '../messaging/submission-result';
import {
  ZLINK_REMOTE_BOUND_SESSION_SEND_PACKET,
  type ZLinkActorRoutedJoinTransport,
  type ZLinkRemoteActorPacketTarget,
  type ZLinkRemoteBoundSessionTarget
} from '../actors';
import { encodeRemoteActorPacketTarget } from '../actors/actor-packet-relay-wire';
import { encodeRemoteBoundSessionSendPayload } from '../actors/bound-session-wire';
import type { ZLinkNativeFallbackBoundSessionPort } from './stream-binding-runtime-ports';
import type { ZLinkBackendActorSessionNode } from '../backend';
import { resolveFrameworkPacketName } from '../messaging/packet-name';
import { currentOrCreateFlow, runWithOutboundFlow } from '../diagnostics/flow-context';

export interface ZLinkNativeFallbackBoundSessionOptions {
  readonly runtime: ZLinkNativeFallbackBoundSessionPort;
  readonly routedTransport: ZLinkActorRoutedJoinTransport;
  readonly actorRefProvider: () => ActorRef | undefined;
  readonly nativeActorNodeProvider: () => ZLinkBackendActorSessionNode | undefined;
  readonly localActorProvider?: () => boolean;
  readonly remoteBoundSessionTargetProvider: () => ZLinkRemoteBoundSessionTarget | undefined;
  readonly remoteActorPacketTargetProvider: () => ZLinkRemoteActorPacketTarget | undefined;
  readonly requestTimeoutMs?: number;
  readonly actorId: string;
  readonly onSend?: (actorId: string, packetName: string) => void;
  readonly reportError: (error: unknown) => void;
  readonly flowCreationEnabled?: () => boolean;
}

export class ZLinkNativeFallbackBoundSession implements ZLinkBoundSession {
  constructor(private readonly options: ZLinkNativeFallbackBoundSessionOptions) {}

  send(message: unknown): ZLinkBoundSessionSendCall {
    return new ZLinkNativeFallbackBoundSessionSendCall(this.options, message);
  }

  async disconnect(signal?: AbortSignal): Promise<void> {
    const actorRef = this.options.actorRefProvider();
    const node = this.options.nativeActorNodeProvider();
    if (
      actorRef !== undefined &&
      node !== undefined &&
      currentBindingGeneration(actorRef) !== undefined
    ) {
      await this.options.runtime.disconnectNativeBoundSession(
        node,
        actorRef,
        this.options.requestTimeoutMs ?? DEFAULT_REQUEST_TIMEOUT_MS,
        signal
      );
      return;
    }
    await this.options.runtime.disconnectBoundSession(this.options.actorId, signal);
  }
}

class ZLinkNativeFallbackBoundSessionSendCall implements ZLinkBoundSessionSendCall {
  private selectedPacketName: string | undefined;
  private readonly selectedMetadata = new Map<string, string>();
  private executed = false;

  constructor(
    private readonly options: ZLinkNativeFallbackBoundSessionOptions,
    private readonly message: unknown
  ) {}

  metadata(key: string, value: string): this {
    this.selectedMetadata.set(key, value);
    return this;
  }

  packetName(packetName: string): this {
    this.selectedPacketName = packetName;
    return this;
  }

  async submit(): Promise<void> {
    if (this.executed) {
      throwAlreadySubmitted('Bound session send call');
    }
    const packetName = resolveFrameworkPacketName(
      this.message,
      this.selectedPacketName,
      'Bound session'
    );
    this.executed = true;
    const result = await this.execute(packetName);
    requireOneWayCompletion(
      result,
      'Bound session send',
      ZLinkFrameworkInternalErrorKind.ActorSessionNotBound
    );
  }

  private execute(packetName: string): Promise<ZLinkSubmitResult> {
    // Call-scoped flow (spec 27 §4): the relay JSON flow pair and any nested
    // routed-transport envelope share one ambient flow that does not outlive
    // this submit call.
    return runWithOutboundFlow(this.options.flowCreationEnabled?.() ?? true, () =>
      this.executeScoped(packetName)
    );
  }

  private async executeScoped(packetName: string): Promise<ZLinkSubmitResult> {
    const localActor = this.options.localActorProvider?.() === true;
    const remoteTarget = this.options.remoteBoundSessionTargetProvider();
    if (localActor || remoteTarget === undefined) {
      const localResult = await this.options.runtime.submitLocalBoundSession(
        this.options.actorId,
        this.message,
        packetName,
        this.selectedMetadata
      );
      if (localResult.status !== ZLinkSubmitStatus.TargetNotFound) {
        this.traceSubmitted(localResult, packetName);
        return localResult;
      }
    }
    if (remoteTarget !== undefined) {
      const actorRef = this.options.actorRefProvider();
      const ownershipGeneration = (
        actorRef as (ActorRef & { ownershipGeneration?: bigint }) | undefined
      )?.ownershipGeneration;
      const payload = encodeRemoteBoundSessionSendPayload({
        actorId: this.options.actorId,
        actorMeshName: actorRef?.meshName,
        actorNodeRid: actorRef === undefined ? undefined : String(actorRef.nodeRid),
        actorNodeRidHex: (actorRef?.nodeRid as { toHex?: () => string } | undefined)?.toHex?.(),
        actorGeneration: actorRef?.objectGeneration.toString(),
        actorOwnershipGeneration: ownershipGeneration?.toString(),
        message: this.message,
        boundPacketName: packetName,
        metadata: this.selectedMetadata,
        actorPacketTarget: encodeRemoteActorPacketTarget(
          this.options.remoteActorPacketTargetProvider()
        ),
        ...((this.options.flowCreationEnabled?.() ?? true)
          ? currentOrCreateFlow('Application')
          : undefined)
      });
      const submit =
        this.options.routedTransport.submitInfrastructure ?? this.options.routedTransport.submit;
      const result =
        submit === undefined
          ? await this.options.routedTransport.sendToSpot(
              {
                routerChannelId: remoteTarget.routerChannelId,
                targetNodeRid: remoteTarget.targetNodeRid,
                spotId: remoteTarget.spotId,
                spotKind: ZLinkSpotKind.Entry
              },
              payload,
              { packetName: ZLINK_REMOTE_BOUND_SESSION_SEND_PACKET }
            )
          : await submit.call(
              this.options.routedTransport,
              remoteTarget.routerChannelId,
              String(remoteTarget.targetNodeRid),
              ZLINK_REMOTE_BOUND_SESSION_SEND_PACKET,
              payload
            );
      this.traceSubmitted(result, packetName);
      return result;
    }
    const actorRef = this.options.actorRefProvider();
    const node = this.options.nativeActorNodeProvider();
    const bindingGeneration = currentBindingGeneration(actorRef);
    if (actorRef !== undefined && node !== undefined && bindingGeneration !== undefined) {
      const result = await this.options.runtime.sendNativeBoundSession(
        node,
        actorRef,
        this.message,
        packetName,
        this.selectedMetadata
      );
      this.traceSubmitted(result, packetName);
      return result;
    }
    if (localActor) {
      throw createInternalFrameworkException(
        ZLinkFrameworkInternalErrorKind.ActorSessionNotBound,
        `No current session binding exists for actor '${this.options.actorId}'.`,
        true
      );
    }
    const result = await this.options.runtime.sendBoundSession(
      this.options.actorId,
      this.message,
      packetName,
      this.selectedMetadata
    );
    this.traceSubmitted(result, packetName);
    return result;
  }

  private traceSubmitted(result: ZLinkSubmitResult, packetName: string): void {
    if (result.status === ZLinkSubmitStatus.Submitted) {
      this.options.onSend?.(this.options.actorId, packetName);
    }
  }
}

function currentBindingGeneration(actorRef: ActorRef | undefined): bigint | undefined {
  const generation = (actorRef as (ActorRef & { readonly bindingGeneration?: bigint }) | undefined)
    ?.bindingGeneration;
  return positiveBindingGeneration(generation);
}

function positiveBindingGeneration(generation: bigint | undefined): bigint | undefined {
  return generation !== undefined && generation > 0n ? generation : undefined;
}
