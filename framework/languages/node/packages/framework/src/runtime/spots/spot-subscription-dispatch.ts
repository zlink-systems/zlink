import type {
  RoutingId,
  Type,
  ZLinkMessageSerializer,
  ZLinkSpot,
  ZLinkSpotSubscriptionHandler
} from '../../contracts';
import type { ZLinkProviderResolver } from '../../contracts/Common/ZLinkProviderResolver';
import { ZLinkFrameworkException, zlinkMessageMetadata } from '../../contracts';
import {
  ZLinkRuntimeDispatchErrorAction as ZLinkDispatchErrorAction,
  ZLinkRuntimeDispatchErrorReason as ZLinkDispatchErrorReason,
  ZLinkDispatchErrorSurface,
  ZLinkDispatchMessageKind
} from '../../contracts/Dispatch/ZLinkDispatchOptions';
import type { Message } from '../../contracts/Common/Message';
import type { ZLinkBackendSpot, ZLinkBackendTopicMessage } from '../backend/contracts';
import type { ZLinkDispatchErrorReporter } from '../channels';
import {
  decodeChannelEnvelope,
  decodeChannelPayload,
  ZLinkChannelMessageKind,
  type ZLinkChannelEnvelopeCodecRegistry
} from '../channels/channel-envelope';
import { createInboundFlow, runWithFlow } from '../diagnostics/flow-context';
import { resolveLifecycleHandler } from '../handlers/handler-instance-scope';
import type { ZLinkSpotHandlerRegistration } from './spot-handler-registry';
import { ZLINK_RECV_DONT_WAIT } from './spot-native-flags';
import type { ZLinkSpotSerialExecutor } from './spot-serial-executor';
import { zlinkMetadataByteLength, zlinkSerialWorkOptions } from '../execution/serial-work-size';
import {
  ZLinkFrameworkInternalErrorKind,
  internalFrameworkErrorKind
} from '../framework-errors-internal';

interface ZLinkSpotSubscriptionDispatchOptions {
  readonly nativeSpot: ZLinkBackendSpot;
  readonly createTopicMessage: () => ZLinkBackendTopicMessage;
  readonly serial: ZLinkSpotSerialExecutor;
  readonly getTarget: () => ZLinkSpot;
  readonly messageSerializers?: ReadonlyMap<string, ZLinkMessageSerializer>;
  readonly providerResolver?: ZLinkProviderResolver;
  readonly dispatchErrors?: ZLinkDispatchErrorReporter;
  readonly waitIdle: () => Promise<void>;
}

export class ZLinkSpotSubscriptionDispatch {
  private draining = false;
  private redrainRequested = false;
  private readonly handlers = new Map<string, ZLinkSpotHandlerRegistration[]>();

  constructor(private readonly options: ZLinkSpotSubscriptionDispatchOptions) {}

  private channelCodecs(): ZLinkChannelEnvelopeCodecRegistry | undefined {
    return this.options.messageSerializers === undefined
      ? undefined
      : { serializers: this.options.messageSerializers };
  }

  configure(registrations: readonly ZLinkSpotHandlerRegistration[]): void {
    for (const registration of registrations) {
      if (registration.kind !== 'subscribe'
        || registration.channelName === undefined
        || registration.topic === undefined) {
        continue;
      }
      const key = subscriptionKey(registration.channelName, registration.topic);
      const existing = this.handlers.get(key) ?? [];
      if (existing.some((entry) => entry.handlerType === registration.handlerType)) {
        continue;
      }
      existing.push(registration);
      this.handlers.set(key, existing);
      this.options.nativeSpot.setSubscription(registration.channelName, registration.topic);
    }
  }

  async drain(): Promise<void> {
    if (this.draining) {
      this.redrainRequested = true;
      return;
    }
    this.draining = true;
    try {
      do {
        this.redrainRequested = false;
        await this.drainAvailable();
        // eslint-disable-next-line @typescript-eslint/no-unnecessary-condition
      } while (this.redrainRequested);
    } finally {
      this.draining = false;
    }
  }

  async dispatchRecord(
    topic: string,
    parts: readonly Message[],
    sourceRid: RoutingId | null
  ): Promise<void> {
    await this.dispatch({
      topic,
      parts,
      routingId: sourceRid
    });
  }

  private async drainAvailable(): Promise<void> {
    let message = this.options.createTopicMessage();
    try {
      for (;;) {
        if (!this.options.nativeSpot.subscribe(message, ZLINK_RECV_DONT_WAIT)) {
          message.close();
          await this.options.waitIdle();
          return;
        }
        try {
          await this.dispatch(message);
        } finally {
          message.close();
        }
        message = this.options.createTopicMessage();
      }
    } finally {
      message.close();
    }
  }

  private async dispatch(message: {
    readonly topic: string;
    readonly parts: readonly Message[];
    readonly routingId: RoutingId | unknown | null;
  }): Promise<void> {
    if (message.parts.length === 0) {
      this.options.dispatchErrors?.report({
        surface: ZLinkDispatchErrorSurface.SpotSubscription,
        messageKind: ZLinkDispatchMessageKind.Publish,
        reason: ZLinkDispatchErrorReason.InvalidFrame,
        action: ZLinkDispatchErrorAction.Drop,
        topic: message.topic,
        sourceRid: message.routingId === null ? undefined : String(message.routingId)
      });
      return;
    }
    const envelope = decodeChannelEnvelope(message.parts);
    if (envelope.header.kind !== ZLinkChannelMessageKind.Publish) {
      this.options.dispatchErrors?.report({
        surface: ZLinkDispatchErrorSurface.SpotSubscription,
        messageKind: ZLinkDispatchMessageKind.Publish,
        reason: ZLinkDispatchErrorReason.InvalidFrame,
        action: ZLinkDispatchErrorAction.Drop,
        packetName: envelope.packetName,
        channelName: envelope.header.channelName,
        topic: message.topic,
        sourceRid: message.routingId === null ? undefined : String(message.routingId),
        correlationId: envelope.header.correlationId ?? undefined
      });
      return;
    }
    const registrations = this.handlers.get(subscriptionKey(envelope.header.channelName, message.topic));
    if (registrations === undefined || registrations.length === 0) {
      this.options.dispatchErrors?.report({
        surface: ZLinkDispatchErrorSurface.SpotSubscription,
        messageKind: ZLinkDispatchMessageKind.Publish,
        reason: ZLinkDispatchErrorReason.HandlerMissing,
        action: ZLinkDispatchErrorAction.Drop,
        packetName: envelope.packetName,
        channelName: envelope.header.channelName,
        topic: message.topic,
        sourceRid: message.routingId === null ? undefined : String(message.routingId),
        correlationId: envelope.header.correlationId ?? undefined
      });
      return;
    }
    const spot = this.options.getTarget();
    const subSource = message.routingId === null ? undefined : String(message.routingId);
    const inboundFlow = createInboundFlow(
      envelope.header.flowId,
      envelope.header.flowOrigin,
      this.options.dispatchErrors?.flow.flowCreationEnabled() ?? true
    );
    try {
      await this.options.serial.execute(() => runWithFlow(inboundFlow, async () => {
        // Payload deserialization belongs to the Spot serial turn. Queue
        // admission and execution authority therefore precede this work.
        const event = decodeChannelPayload(envelope, this.channelCodecs());
        for (const registration of registrations) {
          const handler = await resolveLifecycleHandler(
            spot,
            registration.handlerType as Type<ZLinkSpotSubscriptionHandler<ZLinkSpot, unknown>>,
            this.options.providerResolver
          );
          try {
            await handler.handle(spot, event, {
              channelName: envelope.header.channelName,
              contentType: envelope.header.contentType,
              packetName: envelope.packetName!,
              topic: message.topic,
              source: subSource,
              metadata: zlinkMessageMetadata(envelope.header.metadata),
              correlationId: envelope.header.correlationId ?? undefined
            });
          } catch (error) {
            this.options.dispatchErrors?.report({
              surface: ZLinkDispatchErrorSurface.SpotSubscription,
              messageKind: ZLinkDispatchMessageKind.Publish,
              reason: ZLinkDispatchErrorReason.HandlerException,
              action: ZLinkDispatchErrorAction.Drop,
              packetName: envelope.packetName,
              channelName: envelope.header.channelName,
              topic: message.topic,
              sourceRid: message.routingId === null ? undefined : String(message.routingId),
              correlationId: envelope.header.correlationId ?? undefined,
              error
            });
            throw error;
          }
        }
      }), zlinkSerialWorkOptions(
        envelope.payload.byteLength,
        zlinkMetadataByteLength(envelope.header.metadata)
      ));
    } catch (error) {
      if (error instanceof ZLinkFrameworkException
        && internalFrameworkErrorKind(error) === ZLinkFrameworkInternalErrorKind.PayloadDecodeFailed) {
        this.options.dispatchErrors?.report({
          surface: ZLinkDispatchErrorSurface.SpotSubscription,
          messageKind: ZLinkDispatchMessageKind.Publish,
          reason: ZLinkDispatchErrorReason.PayloadDecodeFailed,
          action: ZLinkDispatchErrorAction.Drop,
          packetName: envelope.packetName,
          channelName: envelope.header.channelName,
          topic: message.topic,
          sourceRid: message.routingId === null ? undefined : String(message.routingId),
          correlationId: envelope.header.correlationId ?? undefined,
          error
        });
      }
      throw error;
    }
  }
}

function subscriptionKey(channelName: string, topic: string): string {
  return `${channelName}\u0000${topic}`;
}
