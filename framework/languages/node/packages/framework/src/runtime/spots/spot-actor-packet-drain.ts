import type {
  ActorRef,
  RoutingId,
  ZLinkMessageSerializer
} from '../../contracts';
import {
  ZLinkFrameworkException,
  ZLinkFrameworkErrorKind
} from '../../contracts';
import type { Message } from '../../contracts/Common/Message';
import { ZLinkBufferMessage as RuntimeMessage } from '../backend/runtime-message';
import { RequestResult } from '../backend/runtime-values';
import type {
  ZLinkBackendActorRef,
  ZLinkBackendActorRecvInfo
} from '../backend/contracts';
import type { ZLinkRemoteBoundSessionTarget } from '../actors';
import { REMOTE_BOUND_SESSION_BIND_PACKET } from './spot-remote-codec';
import { ZLINK_RECV_DONT_WAIT } from './spot-native-flags';
import {
  decodeStreamHeader,
  encodeStreamFrame,
  messageToBytes,
  ZLinkStreamCodec,
  ZLinkStreamHeaderFlags,
  ZLinkStreamMessageKind
} from '../streams/protocol';
import { encodeFrameworkPayloadMessage } from '../messaging/payload-codec';

export interface ZLinkActorDispatchPart {
  readonly info: {
    readonly actor: ZLinkBackendActorRef;
    readonly sourceNodeRid?: RoutingId;
    readonly sourceSessionRid?: RoutingId;
    readonly requestId?: bigint;
    readonly flags?: number;
  };
  readonly message: Message;
  readonly more: boolean;
}

interface ZLinkSpotActorPacketDrainOptions {
  readonly messageSerializers?: ReadonlyMap<string, ZLinkMessageSerializer>;
  readonly actorPacketHandler?: (
    actorId: string,
    parts: readonly Message[],
    returnResponse?: boolean,
    remoteBoundSessionTarget?: ZLinkRemoteBoundSessionTarget,
    fallbackActorRef?: ActorRef
  ) => Promise<unknown>;
  readonly bindRemoteActorSession?: (
    actor: ZLinkBackendActorRef,
    sourceNodeRid: RoutingId,
    sourceSessionRid: RoutingId
  ) => void;
  readonly replyActorNoBind?: (
    info: ZLinkBackendActorRecvInfo,
    parts: readonly Message[],
    result: RequestResult
  ) => void;
  readonly waitIdle: () => Promise<void>;
}

const ZLINK_SPOT_ACTOR_RECV_INFO_NO_BIND = 1;

export class ZLinkSpotActorPacketDrain {
  private readonly continuations = new Map<string, { readonly owner: string; readonly parts: Message[] }>();

  constructor(private readonly options: ZLinkSpotActorPacketDrainOptions) {}

  async drain(info: {
    recvActorPart(flags?: number): ZLinkActorDispatchPart | null;
  }): Promise<void> {
    const parts: Message[] = [];
    let actorId: string | undefined;
    let actorRef: ZLinkBackendActorRef | undefined;
    let sourceNodeRid: RoutingId | undefined;
    let sourceSessionRid: RoutingId | undefined;
    let requestId: bigint | undefined;
    let flags: number | undefined;
    let packetKey: string | undefined;
    try {
      for (;;) {
        const part = info.recvActorPart(ZLINK_RECV_DONT_WAIT);
        if (part === null) {
          if (parts.length > 0 && packetKey !== undefined) {
            this.continuations.set(packetKey, {
              owner: continuationOwner(parts[0]),
              parts: parts.splice(0)
            });
            return;
          }
          await this.options.waitIdle();
          return;
        }
        const currentPacketKey = actorPacketKey(part);
        if (packetKey === undefined) {
          packetKey = currentPacketKey;
          const pending = this.continuations.get(currentPacketKey);
          if (pending !== undefined) {
            if (isStreamHeaderMessage(part.message)) {
              closeMessages(pending.parts);
              this.continuations.delete(currentPacketKey);
              throw new Error(`Actor packet continuation '${pending.owner}' was replaced before completion.`);
            }
            this.continuations.delete(currentPacketKey);
            parts.push(...pending.parts);
          }
        } else if (packetKey !== currentPacketKey) {
          throw new Error('Native actor packet parts changed identity before the final part.');
        }
        actorId ??= part.info.actor.actorId;
        actorRef ??= part.info.actor;
        sourceNodeRid ??= part.info.sourceNodeRid;
        sourceSessionRid ??= part.info.sourceSessionRid;
        requestId ??= part.info.requestId;
        flags ??= part.info.flags;
        parts.push(part.message);
        if (!part.more) {
          break;
        }
      }
      const noBindInfo = this.createNoBindReplyInfo(actorRef, sourceNodeRid, sourceSessionRid, requestId, flags, parts);
      if (this.consumeRemoteBoundSessionBind(actorRef, sourceNodeRid, sourceSessionRid, parts)) {
        return;
      }
      if (
        ((flags ?? 0) & ZLINK_SPOT_ACTOR_RECV_INFO_NO_BIND) === 0 &&
        sourceNodeRid !== undefined &&
        sourceSessionRid !== undefined
      ) {
        this.options.bindRemoteActorSession?.(actorRef, sourceNodeRid, sourceSessionRid);
      }
      if (this.options.actorPacketHandler === undefined) {
        return;
      }
      if (noBindInfo !== undefined) {
        await this.dispatchNoBindActorRequest(noBindInfo, actorId, parts, actorRef);
      } else {
        await this.options.actorPacketHandler(
          actorId,
          parts,
          false,
          undefined,
          actorRef as unknown as ActorRef | undefined
        );
      }
    } finally {
      for (const part of parts) {
        part.close();
      }
    }
  }

  dispose(): void {
    for (const continuation of this.continuations.values()) closeMessages(continuation.parts);
    this.continuations.clear();
  }

  private createNoBindReplyInfo(
    actor: ZLinkBackendActorRef | undefined,
    sourceNodeRid: RoutingId | undefined,
    sourceSessionRid: RoutingId | undefined,
    requestId: bigint | undefined,
    flags: number | undefined,
    parts: readonly Message[]
  ): ZLinkBackendActorRecvInfo | undefined {
    if (
      actor === undefined
      || sourceNodeRid === undefined
      || sourceSessionRid === undefined
      || requestId === undefined
      || requestId === 0n
      || flags === undefined
      || (flags & ZLINK_SPOT_ACTOR_RECV_INFO_NO_BIND) === 0
      || this.options.replyActorNoBind === undefined
      || !this.isActorRequest(parts)
    ) {
      return undefined;
    }
    return { actor, sourceNodeRid, sourceSessionRid, requestId, flags };
  }

  private isActorRequest(parts: readonly Message[]): boolean {
    if (parts.length < 1) {
      return false;
    }
    try {
      const header = decodeStreamHeader(messageToBytes(parts[0]));
      return header.kind === ZLinkStreamMessageKind.Request && header.requestSeq !== undefined;
    } catch {
      return false;
    }
  }

  private async dispatchNoBindActorRequest(
    info: ZLinkBackendActorRecvInfo,
    actorId: string,
    parts: readonly Message[],
    actorRef: ZLinkBackendActorRef | undefined
  ): Promise<void> {
    try {
      const response = await this.options.actorPacketHandler?.(
        actorId,
        parts,
        true,
        undefined,
        actorRef as unknown as ActorRef | undefined
      );
      this.options.replyActorNoBind?.(
        info,
        [this.encodeActorReplyFrame(parts[0], ZLinkStreamMessageKind.Response, response)],
        RequestResult.Ok
      );
    } catch (error) {
      this.options.replyActorNoBind?.(
        info,
        [this.encodeActorReplyFrame(parts[0], ZLinkStreamMessageKind.Error, frameworkErrorPayload(error))],
        RequestResult.Ok
      );
    }
  }

  private encodeActorReplyFrame(
    requestHeaderPart: Message,
    kind: ZLinkStreamMessageKind.Response | ZLinkStreamMessageKind.Error,
    payload: unknown
  ): Message {
    const requestHeader = decodeStreamHeader(messageToBytes(requestHeaderPart));
    const payloadMessage = encodeFrameworkPayloadMessage(payload, this.options.messageSerializers);
    try {
      return RuntimeMessage.from(Buffer.from(encodeStreamFrame({
        kind,
        codec: ZLinkStreamCodec.Json,
        flags: ZLinkStreamHeaderFlags.None,
        requestSeq: requestHeader.requestSeq,
        name: '',
        metadata: new Map(),
        correlationId: requestHeader.correlationId
      }, payloadMessage.data()))) as Message;
    } finally {
      payloadMessage.close();
    }
  }

  private consumeRemoteBoundSessionBind(
    actor: ZLinkBackendActorRef | undefined,
    sourceNodeRid: RoutingId | undefined,
    sourceSessionRid: RoutingId | undefined,
    parts: readonly Message[]
  ): boolean {
    if (parts.length < 1) {
      return false;
    }
    let header: ReturnType<typeof decodeStreamHeader>;
    try {
      header = decodeStreamHeader(messageToBytes(parts[0]));
    } catch {
      return false;
    }
    if (header.name !== REMOTE_BOUND_SESSION_BIND_PACKET) {
      return false;
    }
    if (actor !== undefined && sourceNodeRid !== undefined && sourceSessionRid !== undefined) {
      this.options.bindRemoteActorSession?.(actor, sourceNodeRid, sourceSessionRid);
    }
    return true;
  }
}

function actorPacketKey(part: ZLinkActorDispatchPart): string {
  const info = part.info;
  return [
    String(info.actor.nodeRid),
    info.actor.actorId,
    info.actor.generation.toString(),
    info.sourceNodeRid === undefined ? '' : String(info.sourceNodeRid),
    info.sourceSessionRid === undefined ? '' : String(info.sourceSessionRid),
    info.requestId?.toString() ?? '',
    String(info.flags)
  ].join('\u0000');
}

function continuationOwner(header: Message): string {
  try {
    const decoded = decodeStreamHeader(messageToBytes(header));
    return `${decoded.name}:${decoded.requestSeq?.toString() ?? decoded.flowId ?? 'send'}`;
  } catch {
    return 'invalid-header';
  }
}

function isStreamHeaderMessage(message: Message): boolean {
  try {
    decodeStreamHeader(messageToBytes(message));
    return true;
  } catch {
    return false;
  }
}

function closeMessages(messages: readonly Message[]): void {
  for (const message of messages) message.close();
}

function frameworkErrorPayload(error: unknown): {
  readonly message: string;
  readonly kind: ZLinkFrameworkErrorKind;
} {
  return {
    message: error instanceof Error ? error.message : String(error),
    kind: error instanceof ZLinkFrameworkException
      ? error.kind
      : ZLinkFrameworkErrorKind.InternalFailure
  };
}
