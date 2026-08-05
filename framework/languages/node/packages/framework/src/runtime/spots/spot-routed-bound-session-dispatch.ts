import type { ZLinkBackendReceived as BackendReceived } from '../backend/runtime-values';
import type { ActorRef } from '../../contracts';
import type { ZLinkChannelEnvelopeCodecRegistry } from '../channels/channel-envelope';
import {
  decodeRemoteBoundSessionError,
  decodeRemoteBoundSessionOwnership,
  decodeRemoteBoundSessionResponse,
  decodeRemoteBoundSessionSeal,
  decodeRemoteBoundSessionSend
} from './spot-remote-route-codec';
import {
  isReplyableRequestSeq,
  submitRoutePayloadReply,
  submitRouteReply
} from './spot-route-replies';
import type { ZLinkActorResponseOptions } from './spot-actor-packet-dispatch';
import { createInboundFlow, runWithFlow } from '../diagnostics/flow-context';
import type { ZLinkDispatchErrorReporter } from '../channels';

interface ZLinkSpotRoutedBoundSessionDispatchOptions {
  readonly channelCodecs: () => ZLinkChannelEnvelopeCodecRegistry | undefined;
  readonly routedBoundSessionReceiver?: (
    actorId: string,
    message: unknown,
    packetName: string | undefined,
    metadata: ReadonlyMap<string, string>,
    actorRef?: ActorRef,
    actorPacketTarget?: unknown,
    signal?: AbortSignal
  ) => Promise<void>;
  readonly routedBoundSessionResponseReceiver?: (
    actorId: string,
    packetName: string,
    requestSeq: bigint,
    message: unknown,
    replyOptions: ZLinkActorResponseOptions,
    actorPacketTarget?: unknown,
    signal?: AbortSignal
  ) => Promise<void>;
  readonly routedBoundSessionErrorReceiver?: (
    actorId: string,
    packetName: string,
    requestSeq: bigint,
    error: unknown,
    metadata: ReadonlyMap<string, string>,
    actorPacketTarget?: unknown,
    signal?: AbortSignal
  ) => Promise<void>;
  readonly routedBoundSessionOwnershipReceiver?: (payload: unknown) => Promise<{
    readonly actorId: string;
    readonly actorGeneration: string;
    readonly actorOwnershipGeneration: string;
    readonly bindingGeneration: string;
    readonly targetOwnerLeaseGeneration: string;
    readonly acceptedHighWater: string;
    readonly sealId: string;
  }>;
  readonly routedBoundSessionSealReceiver?: (payload: unknown) => Promise<{
    readonly actorId: string;
    readonly sealId: string;
    readonly acceptedHighWater: string;
  }>;
  readonly dispatchErrors?: ZLinkDispatchErrorReporter;
}

export class ZLinkSpotRoutedBoundSessionDispatch {
  constructor(private readonly options: ZLinkSpotRoutedBoundSessionDispatchOptions) {}

  async dispatch(received: BackendReceived): Promise<boolean> {
    const seal = decodeRemoteBoundSessionSeal(received.parts, this.options.channelCodecs());
    if (seal !== undefined) {
      const ack = await this.options.routedBoundSessionSealReceiver?.(seal);
      if (isReplyableRequestSeq(received.requestSeq)) {
        submitRoutePayloadReply(received, seal.envelope, ack ?? { ok: false });
      }
      return true;
    }
    const ownership = decodeRemoteBoundSessionOwnership(received.parts, this.options.channelCodecs());
    if (ownership !== undefined) {
      const ack = await this.options.routedBoundSessionOwnershipReceiver?.(ownership);
      if (isReplyableRequestSeq(received.requestSeq)) {
        submitRoutePayloadReply(received, ownership.envelope, ack ?? { ok: false });
      }
      return true;
    }
    const boundSessionSend = decodeRemoteBoundSessionSend(received.parts, this.options.channelCodecs());
    if (boundSessionSend !== undefined) {
      await runWithFlow(
        createInboundFlow(
          boundSessionSend.flowId ?? boundSessionSend.envelope?.header.flowId,
          boundSessionSend.flowOrigin ?? boundSessionSend.envelope?.header.flowOrigin,
          this.options.dispatchErrors?.flow.flowCreationEnabled() ?? true
        ),
        () => this.options.routedBoundSessionReceiver?.(
          boundSessionSend.actorId,
          boundSessionSend.message,
          boundSessionSend.packetName,
          boundSessionSend.metadata,
          boundSessionSend.actorRef,
          boundSessionSend.actorPacketTarget
        )
      );
      if (isReplyableRequestSeq(received.requestSeq)) {
        submitRoutePayloadReply(received, boundSessionSend.envelope, { ok: true });
      }
      return true;
    }

    const boundSessionResponse = decodeRemoteBoundSessionResponse(received.parts, this.options.channelCodecs());
    if (boundSessionResponse !== undefined) {
      await this.options.routedBoundSessionResponseReceiver?.(
        boundSessionResponse.actorId,
        boundSessionResponse.packetName,
        boundSessionResponse.requestSeq,
        boundSessionResponse.message,
        {
          metadata: boundSessionResponse.metadata,
          compressPayload: boundSessionResponse.compressPayload
        },
        boundSessionResponse.actorPacketTarget
      );
      this.replyOk(received);
      return true;
    }

    const boundSessionError = decodeRemoteBoundSessionError(received.parts, this.options.channelCodecs());
    if (boundSessionError !== undefined) {
      await this.options.routedBoundSessionErrorReceiver?.(
        boundSessionError.actorId,
        boundSessionError.packetName,
        boundSessionError.requestSeq,
        boundSessionError.error,
        boundSessionError.metadata,
        boundSessionError.actorPacketTarget
      );
      this.replyOk(received);
      return true;
    }

    return false;
  }

  private replyOk(received: BackendReceived): void {
    if (!isReplyableRequestSeq(received.requestSeq)) {
      return;
    }
    submitRouteReply(
      received.reply()
        .message(Buffer.from(JSON.stringify({ ok: true })))
    );
  }
}
