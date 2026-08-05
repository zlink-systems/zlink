import type {
  Type,
  ZLinkMessageSerializer,
  ZLinkSpot,
  ZLinkSpotPacketHandler,
  ZLinkSpotRequestHandler
} from '../../contracts';
import type { ZLinkProviderResolver } from '../../contracts/Common/ZLinkProviderResolver';
import { ZLinkFrameworkException, zlinkMessageMetadata } from '../../contracts';
import {
  ZLinkRuntimeDispatchErrorAction as ZLinkDispatchErrorAction,
  ZLinkRuntimeDispatchErrorReason as ZLinkDispatchErrorReason,
  ZLinkDispatchErrorSurface,
  ZLinkDispatchMessageKind
} from '../../contracts/Dispatch/ZLinkDispatchOptions';
import { ZLinkBufferMessage as RuntimeMessage } from '../backend/runtime-message';
import type {
  ZLinkBackendReceived as BackendReceived
} from '../backend/runtime-values';
import type { Message } from '../../contracts/Common/Message';
import type { ZLinkDispatchErrorReporter } from '../channels';
import {
  decodeChannelEnvelope,
  decodeChannelPayload,
  encodeChannelErrorReplyParts,
  encodeChannelReplyParts,
  ZLinkChannelMessageKind,
  type ZLinkChannelEnvelopeCodecRegistry
} from '../channels/channel-envelope';
import {
  ZLINK_REMOTE_ACTOR_PACKET_RELAY_PACKET,
  ZLINK_REMOTE_BOUND_SESSION_ERROR_PACKET,
  ZLINK_REMOTE_BOUND_SESSION_RESPONSE_PACKET,
  ZLINK_REMOTE_BOUND_SESSION_SEND_PACKET
} from '../actors';
import { resolveLifecycleHandler } from '../handlers/handler-instance-scope';
import type { ZLinkSpotHandlerRegistration } from './spot-handler-registry';
import type { ZLinkSpotSerialExecutor } from './spot-serial-executor';
import type { ZLinkApplicationWorkClaim } from '../admission';
import { zlinkMetadataByteLength, zlinkSerialWorkOptions } from '../execution/serial-work-size';
import { REMOTE_ACTOR_JOIN_PACKET } from './spot-remote-codec';
import {
  appendRouteReplyParts,
  isReplyableRequestSeq,
  submitRouteReply
} from './spot-route-replies';
import { createInboundFlow, runWithFlow } from '../diagnostics/flow-context';
import {
  ZLinkFrameworkInternalErrorKind,
  internalFrameworkErrorKind
} from '../framework-errors-internal';

interface ZLinkSpotRoutePacketDispatchOptions {
  readonly packetHandlers: ReadonlyMap<string, readonly ZLinkSpotHandlerRegistration[]>;
  readonly nativeSpotId: string;
  readonly serial: ZLinkSpotSerialExecutor;
  readonly getTarget: () => ZLinkSpot;
  readonly providerResolver?: ZLinkProviderResolver;
  readonly messageSerializers?: ReadonlyMap<string, ZLinkMessageSerializer>;
  readonly dispatchErrors?: ZLinkDispatchErrorReporter;
  readonly claimApplicationWork?: () => ZLinkApplicationWorkClaim;
}

const SPOT_DIRECT_ENVELOPE = 'zlink.framework.spot-direct.v1';

interface ZLinkSpotDirectEnvelope {
  readonly kind: ZLinkChannelMessageKind.Request | ZLinkChannelMessageKind.Command;
  readonly channelName: string;
  readonly packetName?: string;
  readonly payload: unknown;
  readonly metadata: Readonly<Record<string, string>>;
}

export class ZLinkSpotRoutePacketDispatch {
  constructor(private readonly options: ZLinkSpotRoutePacketDispatchOptions) {}

  async dispatch(received: BackendReceived): Promise<boolean> {
    const directEnvelope = decodeSpotDirectEnvelope(received.parts);
    if (directEnvelope !== undefined) {
      await this.dispatchSpotDirectEnvelope(received, directEnvelope);
      return true;
    }
    let envelope: ReturnType<typeof decodeChannelEnvelope>;
    try {
      envelope = decodeChannelEnvelope(received.parts);
    } catch {
      return false;
    }
    if (
      envelope.header.kind !== ZLinkChannelMessageKind.Request &&
      envelope.header.kind !== ZLinkChannelMessageKind.Command
    ) {
      return false;
    }
    if (
      envelope.packetName === REMOTE_ACTOR_JOIN_PACKET ||
      envelope.packetName === ZLINK_REMOTE_ACTOR_PACKET_RELAY_PACKET ||
      envelope.packetName === ZLINK_REMOTE_BOUND_SESSION_SEND_PACKET ||
      envelope.packetName === ZLINK_REMOTE_BOUND_SESSION_RESPONSE_PACKET ||
      envelope.packetName === ZLINK_REMOTE_BOUND_SESSION_ERROR_PACKET
    ) {
      return false;
    }
    const registrations = this.options.packetHandlers.get(envelope.packetName ?? '');
    const replyable = isReplyableRequestSeq(received.requestSeq);
    const action = replyable
      ? ZLinkDispatchErrorAction.ReplyError
      : ZLinkDispatchErrorAction.Drop;
    if (registrations === undefined || registrations.length === 0) {
      this.options.dispatchErrors?.report({
        surface: ZLinkDispatchErrorSurface.SpotRoute,
        messageKind: replyable ? ZLinkDispatchMessageKind.Request : ZLinkDispatchMessageKind.Send,
        reason: ZLinkDispatchErrorReason.HandlerMissing,
        action,
        packetName: envelope.packetName,
        channelName: envelope.header.channelName,
        spotId: this.options.nativeSpotId,
        sourceRid: received.routingId === null ? undefined : String(received.routingId),
        correlationId: envelope.header.correlationId ?? received.requestSeq?.toString(),
        flowId: envelope.header.flowId,
        flowOrigin: envelope.header.flowOrigin
      });
      if (replyable) {
        submitRouteReply(appendRouteReplyParts(
          received.reply(),
          encodeChannelErrorReplyParts(envelope.header, `SPOT route handler not found: ${envelope.packetName}`)
        ));
      }
      return true;
    }
    const context = {
      channelName: envelope.header.channelName,
      contentType: envelope.header.contentType,
      packetName: envelope.packetName!,
      metadata: zlinkMessageMetadata(envelope.header.metadata),
      correlationId: envelope.header.correlationId ?? received.requestSeq?.toString()
    };
    try {
      let response: unknown;
      const applicationClaim = this.options.claimApplicationWork?.();
      try {
        await runWithFlow(createInboundFlow(
          envelope.header.flowId,
          envelope.header.flowOrigin,
          this.options.dispatchErrors?.flow.flowCreationEnabled() ?? true
        ), () =>
          this.options.serial.execute(async () => {
            const spot = this.options.getTarget();
            // Keep the wire payload raw until this Spot has acquired its
            // execution authority. A rejected or superseded work item must not
            // pay the deserialization cost.
            const payload = decodeChannelPayload(envelope, this.channelCodecs());
            for (const registration of registrations) {
              const handler = await resolveLifecycleHandler(
                spot,
                registration.handlerType as Type<ZLinkSpotPacketHandler<ZLinkSpot, unknown> | ZLinkSpotRequestHandler<ZLinkSpot, unknown, unknown>>,
                this.options.providerResolver
              );
              response = await handler.handle(spot, payload, context);
            }
          }, zlinkSerialWorkOptions(
            envelope.payload.byteLength,
            zlinkMetadataByteLength(envelope.header.metadata)
          )));
      } finally {
        applicationClaim?.close();
      }
      if (replyable) {
        submitRouteReply(appendRouteReplyParts(
          received.reply(),
          encodeChannelReplyParts(envelope.header, response)
        ));
      }
      return true;
    } catch (error) {
      this.options.dispatchErrors?.report({
        surface: ZLinkDispatchErrorSurface.SpotRoute,
        messageKind: replyable ? ZLinkDispatchMessageKind.Request : ZLinkDispatchMessageKind.Send,
        reason: error instanceof ZLinkFrameworkException
          && internalFrameworkErrorKind(error) === ZLinkFrameworkInternalErrorKind.PayloadDecodeFailed
          ? ZLinkDispatchErrorReason.PayloadDecodeFailed
          : ZLinkDispatchErrorReason.HandlerException,
        action,
        packetName: envelope.packetName,
        channelName: envelope.header.channelName,
        spotId: this.options.nativeSpotId,
        sourceRid: received.routingId === null ? undefined : String(received.routingId),
        correlationId: envelope.header.correlationId ?? received.requestSeq?.toString(),
        flowId: envelope.header.flowId,
        flowOrigin: envelope.header.flowOrigin,
        error
      });
      if (replyable) {
        submitRouteReply(appendRouteReplyParts(
          received.reply(),
          encodeChannelErrorReplyParts(envelope.header, error)
        ));
      }
      return true;
    }
  }

  private async dispatchSpotDirectEnvelope(
    received: BackendReceived,
    envelope: ZLinkSpotDirectEnvelope
  ): Promise<void> {
    const replyable = envelope.kind === ZLinkChannelMessageKind.Request && isReplyableRequestSeq(received.requestSeq);
    const messageKind = replyable ? ZLinkDispatchMessageKind.Request : ZLinkDispatchMessageKind.Send;
    const action = replyable ? ZLinkDispatchErrorAction.ReplyError : ZLinkDispatchErrorAction.Drop;
    const registrations = this.options.packetHandlers.get(envelope.packetName ?? '');
    if (registrations === undefined || registrations.length === 0) {
      this.options.dispatchErrors?.report({
        surface: ZLinkDispatchErrorSurface.SpotRoute,
        messageKind,
        reason: ZLinkDispatchErrorReason.HandlerMissing,
        action,
        packetName: envelope.packetName,
        channelName: envelope.channelName
      });
      if (replyable) {
        submitRouteReply(
          received.reply()
            .message(encodeSpotDirectReply(false, undefined, `SPOT route handler not found: ${envelope.packetName}`))
        );
      }
      return;
    }
    try {
      let response: unknown;
      await this.options.serial.execute(async () => {
        const spot = this.options.getTarget();
        for (const registration of registrations) {
          const handler = await resolveLifecycleHandler(
            spot,
            registration.handlerType as Type<ZLinkSpotPacketHandler<ZLinkSpot, unknown> | ZLinkSpotRequestHandler<ZLinkSpot, unknown, unknown>>,
            this.options.providerResolver
          );
          response = await handler.handle(spot, envelope.payload, {
            channelName: envelope.channelName,
            packetName: envelope.packetName!,
            metadata: zlinkMessageMetadata(envelope.metadata),
            correlationId: received.requestSeq?.toString()
          });
        }
      }, zlinkSerialWorkOptions(
        Buffer.byteLength(JSON.stringify(envelope.payload ?? null), 'utf8'),
        zlinkMetadataByteLength(envelope.metadata)
      ));
      if (replyable) {
        submitRouteReply(
          received.reply()
            .message(encodeSpotDirectReply(true, response))
        );
      }
    } catch (error) {
      this.options.dispatchErrors?.report({
        surface: ZLinkDispatchErrorSurface.SpotRoute,
        messageKind,
        reason: ZLinkDispatchErrorReason.HandlerException,
        action,
        packetName: envelope.packetName,
        channelName: envelope.channelName,
        error
      });
      if (replyable) {
        submitRouteReply(
          received.reply()
            .message(encodeSpotDirectReply(false, undefined, error instanceof Error ? error.message : String(error)))
        );
        return;
      }
      throw error;
    }
  }

  private channelCodecs(): ZLinkChannelEnvelopeCodecRegistry | undefined {
    return this.options.messageSerializers === undefined
      ? undefined
      : { serializers: this.options.messageSerializers };
  }
}

function decodeSpotDirectEnvelope(parts: readonly Message[]): ZLinkSpotDirectEnvelope | undefined {
  if (parts.length !== 1) {
    return undefined;
  }
  try {
    const decoded = JSON.parse(parts[0].data().toString()) as {
      readonly marker?: unknown;
      readonly kind?: unknown;
      readonly channelName?: unknown;
      readonly packetName?: unknown;
      readonly payload?: unknown;
      readonly metadata?: unknown;
    };
    if (
      decoded.marker !== SPOT_DIRECT_ENVELOPE ||
      (decoded.kind !== ZLinkChannelMessageKind.Request && decoded.kind !== ZLinkChannelMessageKind.Command) ||
      typeof decoded.channelName !== 'string'
    ) {
      return undefined;
    }
    const metadata = decoded.metadata ?? {};
    if (
      typeof metadata !== 'object'
      || Array.isArray(metadata)
      || !Object.values(metadata).every(value => typeof value === 'string')
    ) {
      return undefined;
    }
    return {
      kind: decoded.kind,
      channelName: decoded.channelName,
      packetName: typeof decoded.packetName === 'string' ? decoded.packetName : undefined,
      payload: decoded.payload,
      metadata: metadata as Readonly<Record<string, string>>
    };
  } catch {
    return undefined;
  }
}

function encodeSpotDirectReply(ok: boolean, response?: unknown, error?: string): Message {
  return RuntimeMessage.from(Buffer.from(JSON.stringify({
    marker: SPOT_DIRECT_ENVELOPE,
    ok,
    response,
    error
  })));
}
