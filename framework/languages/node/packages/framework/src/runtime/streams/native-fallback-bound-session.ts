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
  ZLINK_REMOTE_ACTOR_PACKET_RELAY_PACKET,
  ZLINK_REMOTE_ACTOR_SESSION_DISCONNECTED_PACKET,
  ZLINK_REMOTE_BOUND_SESSION_SEND_PACKET,
  type ZLinkActorRoutedJoinTransport,
  type ZLinkRemoteActorPacketTarget,
  type ZLinkRemoteBoundSessionTarget
} from '../actors';
import {
  encodeRemoteActorPacketRelayPayload,
  encodeRemoteActorPacketTarget
} from '../actors/actor-packet-relay-wire';
import { encodeRemoteBoundSessionSendPayload } from '../actors/bound-session-wire';
import { tryRequestRoutedJson } from '../actors/actor-routed-json-request';
import {
  ZLinkStreamCodec,
  ZLinkStreamHeaderFlags,
  ZLinkStreamMessageKind,
  encodeStreamHeader,
  type ZLinkStreamFrameHeader
} from './protocol';
import type { ZLinkNativeFallbackBoundSessionPort } from './stream-binding-runtime-ports';
import { throwIfAborted } from '../abort';
import type { ZLinkBackendActorSessionNode } from '../backend';
import { resolveFrameworkPacketName } from '../messaging/packet-name';
import { currentOrCreateFlow } from '../diagnostics/flow-context';

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
    const remoteTarget = this.options.remoteBoundSessionTargetProvider();
    if (remoteTarget !== undefined) {
      const spotKind: ZLinkSpotKind | undefined =
        'spotKind' in remoteTarget ? remoteTarget.spotKind as ZLinkSpotKind | undefined : undefined;
      const payload = encodeRemoteActorPacketRelayPayload({
        actorId: this.options.actorId,
        routerChannelId: remoteTarget.routerChannelId,
        header: encodeStreamHeader(disconnectedFrameHeader()),
        payload: Buffer.alloc(0)
      });
      const target = {
        routerChannelId: remoteTarget.routerChannelId,
        targetNodeRid: remoteTarget.targetNodeRid,
        spotId: remoteTarget.spotId,
        spotKind: spotKind ?? ZLinkSpotKind.Entry
      };
      if (await tryRequestRoutedJson(
        this.options.routedTransport,
        target,
        payload,
        { timeoutMs: this.options.requestTimeoutMs, signal }
      )) {
        return;
      }
      await this.options.routedTransport.sendToSpot(
        target,
        payload,
        { packetName: ZLINK_REMOTE_ACTOR_PACKET_RELAY_PACKET, signal }
      );
      return;
    }
    const actorRef = this.options.actorRefProvider();
    const node = this.options.nativeActorNodeProvider();
    if (
      actorRef !== undefined
      && node !== undefined
      && currentBindingGeneration(actorRef) !== undefined
    ) {
      await this.options.runtime.disconnectNativeBoundSession(node, actorRef, signal);
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

  async submit(signal?: AbortSignal): Promise<void> {
    if (this.executed) {
      throwAlreadySubmitted('Bound session send call');
    }
    const packetName = resolveFrameworkPacketName(this.message, this.selectedPacketName, 'Bound session');
    this.executed = true;
    throwIfAborted(signal);
    const result = await this.execute(packetName, signal);
    requireOneWayCompletion(
      result,
      'Bound session send',
      ZLinkFrameworkInternalErrorKind.ActorSessionNotBound
    );
  }

  private async execute(packetName: string, signal?: AbortSignal): Promise<ZLinkSubmitResult> {
    const localActor = this.options.localActorProvider?.() === true;
    const remoteTarget = this.options.remoteBoundSessionTargetProvider();
    let nativeAttempted = false;
    if (localActor) {
      const result = await this.options.runtime.submitLocalBoundSession(
        this.options.actorId,
        this.message,
        packetName,
        this.selectedMetadata,
        signal
      );
      if (result.status !== ZLinkSubmitStatus.TargetNotFound) {
        this.traceSubmitted(result, packetName);
        return result;
      }

      // A local Actor with a concrete native owner has a direct binding path.
      // Use that path before a routed transfer target. The transfer target is
      // retained for actors whose current owner is remote or whose native
      // binding is no longer available.
      const actorRef = this.options.actorRefProvider();
      const node = this.options.nativeActorNodeProvider();
      const bindingGeneration = currentBindingGeneration(actorRef);
      if (
        actorRef !== undefined
        && node !== undefined
        && bindingGeneration !== undefined
      ) {
        nativeAttempted = true;
        const result = await this.options.runtime.sendNativeBoundSession(
          node,
          actorRef,
          this.message,
          packetName,
          this.selectedMetadata,
          signal
        );
        if (result.status !== ZLinkSubmitStatus.TargetNotFound) {
          this.traceSubmitted(result, packetName);
          return result;
        }
        if (remoteTarget === undefined) {
          return result;
        }
      }
    }
    if (remoteTarget !== undefined) {
      const actorRef = this.options.actorRefProvider();
      const ownershipGeneration = (actorRef as (ActorRef & { ownershipGeneration?: bigint }) | undefined)
        ?.ownershipGeneration;
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
        ...(currentOrCreateFlow(
          'Application',
          this.options.flowCreationEnabled?.() ?? true
        ) ?? {})
      });
      const submit = this.options.routedTransport.submitInfrastructure
        ?? this.options.routedTransport.submit;
      const result = submit === undefined
        ? await this.options.routedTransport.sendToSpot(
            {
              routerChannelId: remoteTarget.routerChannelId,
              targetNodeRid: remoteTarget.targetNodeRid,
              spotId: remoteTarget.spotId,
              spotKind: ZLinkSpotKind.Entry
            },
            payload,
            { packetName: ZLINK_REMOTE_BOUND_SESSION_SEND_PACKET, signal }
          )
        : await submit.call(
            this.options.routedTransport,
            remoteTarget.routerChannelId,
            String(remoteTarget.targetNodeRid),
            ZLINK_REMOTE_BOUND_SESSION_SEND_PACKET,
            payload,
            signal
          );
      this.traceSubmitted(result, packetName);
      return result;
    }
    const localResult = await this.options.runtime.submitLocalBoundSession(
      this.options.actorId,
      this.message,
      packetName,
      this.selectedMetadata,
      signal
    );
    if (localResult.status !== ZLinkSubmitStatus.TargetNotFound) {
      this.traceSubmitted(localResult, packetName);
      return localResult;
    }
    const actorRef = this.options.actorRefProvider();
    const node = this.options.nativeActorNodeProvider();
    const bindingGeneration = currentBindingGeneration(actorRef);
    if (
      !nativeAttempted
      && actorRef !== undefined
      && node !== undefined
      && bindingGeneration !== undefined
    ) {
      const result = await this.options.runtime.sendNativeBoundSession(
        node,
        actorRef,
        this.message,
        packetName,
        this.selectedMetadata,
        signal
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
      this.selectedMetadata,
      signal
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

function disconnectedFrameHeader(): ZLinkStreamFrameHeader {
  return {
    kind: ZLinkStreamMessageKind.Send,
    codec: ZLinkStreamCodec.Raw,
    flags: ZLinkStreamHeaderFlags.None,
    name: ZLINK_REMOTE_ACTOR_SESSION_DISCONNECTED_PACKET,
    metadata: new Map()
  };
}
