import type {
  RoutingId,
  ZLinkMessageContext,
  ZLinkHandlerFilter,
  ZLinkPublishMessageContext,
  ZLinkRouteMessageContext,
  ZLinkUnhandledDispatchOptions
} from '../../contracts';
import { zlinkMessageMetadata } from '../../contracts';
import {
  ZLinkDispatchErrorSurface,
  ZLinkDispatchMessageKind
} from '../../contracts/Dispatch/ZLinkDispatchOptions';
import type { Message } from '../../contracts/Common/Message';
import { SubmitResult } from '../backend/runtime-values';
import type { ReceiveRecord } from '../foundation/service-runtime-contracts';
import {
  ZLinkConfigurationException,
  type ZLinkRouteChannelOptions
} from '../configuration';
import type { ZLinkBackendSpotRouteBridge } from '../backend/contracts';
import type { ZLinkRuntimeMetrics } from '../diagnostics';
import {
  decodeChannelEnvelope,
  encodeChannelErrorReplyParts,
  encodeChannelReplyParts,
  type ZLinkChannelEnvelopeCodecRegistry,
  ZLinkChannelMessageKind,
  type ZLinkChannelEnvelopeHeader
} from './channel-envelope';
import { ZLinkDispatchErrorReporter } from './dispatch-error-reporter';
import {
  inspectMalformedChannelHeader,
  malformedProtocolErrorRequestHeader,
  tryDecodeChannelHeader
} from './channel-envelope-inspection';
import {
  appendParts,
  type ZLinkMultipartOperation,
  type ZLinkMultipartReplyOperation,
  type ZLinkMultipartSubmitOperation
} from './channel-multipart';
import { codecsForFrameworkPacket } from './channel-framework-packets';
import {
  ZLinkChannelDispatchPipeline,
  type ZLinkChannelDispatchFields
} from './channel-dispatch-pipeline';

export interface ZLinkChannelRequestDispatcherOptions {
  readonly meshName?: string;
  readonly channelName: string;
  /**
   * Mesh channels use the routed message context so a handler can associate
   * a report with the node that sent it.
   */
  readonly routeMesh?: boolean;
  readonly codecs?: ZLinkChannelEnvelopeCodecRegistry;
  readonly dispatchErrors: ZLinkDispatchErrorReporter;
  readonly handlers: ReadonlyMap<string, ZLinkChannelRequestHandler>;
  readonly sendHandlers?: ReadonlyMap<string, ZLinkChannelSendHandler>;
  readonly filters?: readonly ZLinkHandlerFilter[];
  readonly unhandled?: ZLinkUnhandledDispatchOptions;
}

export interface ZLinkChannelRequestHandler {
  handle(payload: unknown, context: ZLinkMessageContext): Promise<unknown> | unknown;
}

export interface ZLinkChannelSendHandler {
  handle(payload: unknown, context: ZLinkMessageContext): Promise<void> | void;
}

export interface ZLinkRouteHandlerRegistration {
  readonly kind: 'send' | 'request';
  readonly packetName: string;
  readonly handler: ZLinkRouteRuntimeSendHandler | ZLinkRouteRuntimeRequestHandler;
}

export interface ZLinkRouteRuntimeSendHandler {
  handle(payload: unknown, context: ZLinkRouteMessageContext): Promise<void> | void;
}

export interface ZLinkRouteRuntimeRequestHandler {
  handle(payload: unknown, context: ZLinkRouteMessageContext): Promise<unknown> | unknown;
}

export class ZLinkChannelRequestDispatcher {
  private readonly pipeline: ZLinkChannelDispatchPipeline;

  constructor(private readonly options: ZLinkChannelRequestDispatcherOptions) {
    this.pipeline = new ZLinkChannelDispatchPipeline({
      dispatchErrors: options.dispatchErrors,
      surface: ZLinkDispatchErrorSurface.Channel,
      channelName: options.channelName,
      filters: options.filters,
      unhandled: options.unhandled
    });
  }

  /** Spec 27 §4: with tracing Off, inbound flow fields are neither validated nor carried forward. */
  flowEnabled(): boolean {
    return this.options.dispatchErrors.flow.flowCreationEnabled();
  }

  async dispatch(received: {
    parts: readonly Message[];
    routingId: unknown;
    spotId?: unknown;
    requestSeq: bigint | null;
    send?: () => ZLinkMultipartOperation<ZLinkMultipartSubmitOperation>;
  }, router: {
    reply(routingId: unknown, requestSeq: bigint): ZLinkMultipartReplyOperation;
  }, signal?: AbortSignal, decodedHeader?: ZLinkChannelEnvelopeHeader): Promise<boolean | void> {
    if (received.spotId !== null && received.spotId !== undefined) {
      if (received.send === undefined) {
        throw new ZLinkConfigurationException('Routed SPOT packet is missing a local SPOT delivery context.');
      }
      appendParts(received.send(), received.parts).submit();
      return true;
    }
    if (received.parts.length === 0 || received.parts[0].data().length === 0) {
      return;
    }
    let envelope: ReturnType<typeof decodeChannelEnvelope>;
    try {
      envelope = decodeChannelEnvelope(received.parts, decodedHeader, this.flowEnabled());
    } catch (error) {
      const requestSeq = received.requestSeq;
      const info = inspectMalformedChannelHeader(received.parts, this.flowEnabled());
      await this.pipeline.dispatchMalformed({
        info,
        error,
        transportRequest: requestSeq !== null,
        writeProtocolError: requestSeq === null
          ? undefined
          : protocolError => this.submitReply(
              appendParts(
                router.reply(received.routingId, requestSeq),
                encodeChannelErrorReplyParts(
                  malformedProtocolErrorRequestHeader(this.options.channelName, info),
                  protocolError
                )
              ),
              signal
            )
      });
      return;
    }
    const packetName = envelope.packetName;
    if (packetName === undefined) {
      throw new ZLinkConfigurationException('Channel packet is missing packetName.');
    }
    const correlationId = envelope.header.correlationId ?? undefined;
    if (envelope.header.kind === ZLinkChannelMessageKind.Command) {
      const fields: ZLinkChannelDispatchFields = {
        messageKind: ZLinkDispatchMessageKind.Send,
        packetName,
        correlationId,
        flowId: envelope.header.flowId,
        flowOrigin: envelope.header.flowOrigin
      };
      const handler = this.options.sendHandlers?.get(packetName);
      const context: ZLinkMessageContext = {
        meshName: this.options.meshName,
        channelName: this.options.channelName,
        contentType: envelope.header.contentType,
        packetName,
        metadata: zlinkMessageMetadata(envelope.header.metadata),
        correlationId
      };
      await this.pipeline.dispatchOneWay({
        fields,
        envelope,
        codecs: this.options.codecs,
        handler,
        context,
        signal
      });
      return;
    }
    if (envelope.header.kind !== ZLinkChannelMessageKind.Request) {
      return;
    }
    const requestFields: ZLinkChannelDispatchFields = {
      messageKind: ZLinkDispatchMessageKind.Request,
      packetName,
      correlationId,
      flowId: envelope.header.flowId,
      flowOrigin: envelope.header.flowOrigin
    };
    if (received.requestSeq === null) {
      this.pipeline.dropMissingReplyPath(requestFields);
      return;
    }

    const requestSeq = received.requestSeq;
    const handler = this.options.handlers.get(packetName);
    const context: ZLinkMessageContext = {
      meshName: this.options.meshName,
      channelName: this.options.channelName,
      contentType: envelope.header.contentType,
      packetName,
      metadata: zlinkMessageMetadata(envelope.header.metadata),
      correlationId
    };
    await this.pipeline.dispatchRequest({
      fields: requestFields,
      envelope,
      codecs: this.options.codecs,
      handler,
      context,
      signal,
      missingHandlerMessage: `No channel request handler is registered for '${this.options.channelName}:${packetName}'.`,
      writeReply: reply => this.submitReply(
        appendParts(
          router.reply(received.routingId, requestSeq),
          encodeChannelReplyParts(envelope.header, reply, this.options.codecs)
        ),
        signal
      ),
      writeError: error => this.submitReply(
        appendParts(
          router.reply(received.routingId, requestSeq),
          encodeChannelErrorReplyParts(envelope.header, error)
        ),
        signal
      )
    });
  }

  async dispatchMesh(record: ReceiveRecord, signal?: AbortSignal): Promise<void> {
    if (record.parts.length === 0 || record.parts[0].data().length === 0) {
      return;
    }
    let envelope: ReturnType<typeof decodeChannelEnvelope>;
    try {
      envelope = decodeChannelEnvelope(record.parts, undefined, this.flowEnabled());
    } catch (error) {
      const info = inspectMalformedChannelHeader(record.parts, this.flowEnabled());
      await this.pipeline.dispatchMalformed({
        info,
        error,
        transportRequest: false,
        sourceRid: record.sourceNodeRid == null ? undefined : String(record.sourceNodeRid),
        writeProtocolError: async (protocolError) => {
          requireMeshReplyAccepted(record.reply(encodeChannelErrorReplyParts(
            malformedProtocolErrorRequestHeader(this.options.channelName, info),
            protocolError
          )));
        }
      });
      return;
    }
    const packetName = envelope.packetName;
    if (packetName === undefined) {
      throw new ZLinkConfigurationException('Channel packet is missing packetName.');
    }
    const correlationId = envelope.header.correlationId ?? undefined;
    if (envelope.header.kind === ZLinkChannelMessageKind.Command) {
      await this.pipeline.dispatchOneWay({
        fields: {
          messageKind: ZLinkDispatchMessageKind.Send,
          packetName,
          correlationId,
          flowId: envelope.header.flowId,
          flowOrigin: envelope.header.flowOrigin
        },
        envelope,
        codecs: this.options.codecs,
        handler: this.options.sendHandlers?.get(packetName),
        context: this.createMeshContext(envelope, record.sourceNodeRid),
        signal
      });
      return;
    }
    if (envelope.header.kind !== ZLinkChannelMessageKind.Request) {
      return;
    }
    const fields: ZLinkChannelDispatchFields = {
      messageKind: ZLinkDispatchMessageKind.Request,
      packetName,
      correlationId,
      flowId: envelope.header.flowId,
      flowOrigin: envelope.header.flowOrigin
    };
    await this.pipeline.dispatchRequest({
      fields,
      envelope,
      codecs: this.options.codecs,
      handler: this.options.handlers.get(packetName),
      context: this.createMeshContext(envelope, record.sourceNodeRid),
      signal,
      missingHandlerMessage: `No channel request handler is registered for '${this.options.channelName}:${packetName}'.`,
      writeReply: async (reply) => {
        requireMeshReplyAccepted(record.reply(
          encodeChannelReplyParts(envelope.header, reply, this.options.codecs)
        ));
      },
      writeError: async (error) => {
        requireMeshReplyAccepted(record.reply(
          encodeChannelErrorReplyParts(envelope.header, error)
        ));
      }
    });
  }

  private submitReply(operation: ZLinkMultipartReplyOperation, signal?: AbortSignal): Promise<void> {
    void signal;
    if (operation.submit() === false) {
      throw new ZLinkConfigurationException('Channel reply was not accepted by Core.');
    }
    return Promise.resolve();
  }

  private createMeshContext(
    envelope: ReturnType<typeof decodeChannelEnvelope>,
    sourceNodeRid: unknown
  ): ZLinkMessageContext {
    const base = {
      meshName: this.options.meshName,
      channelName: this.options.channelName,
      contentType: envelope.header.contentType,
      packetName: envelope.packetName!,
      metadata: zlinkMessageMetadata(envelope.header.metadata),
      correlationId: envelope.header.correlationId ?? undefined
    };
    if (this.options.routeMesh !== true) {
      return base;
    }
    return {
      ...base,
      meshName: this.options.meshName!,
      sourceNodeRid: String(sourceNodeRid ?? '')
    } as ZLinkRouteMessageContext;
  }
}

function requireMeshReplyAccepted(result: number): void {
  if (result !== SubmitResult.Ok) {
    throw new ZLinkConfigurationException(`MeshNode channel reply was not accepted (submit result ${result}).`);
  }
}

export interface ZLinkChannelPublishDispatcherOptions {
  readonly meshName?: string;
  readonly channelName: string;
  readonly codecs?: ZLinkChannelEnvelopeCodecRegistry;
  readonly dispatchErrors: ZLinkDispatchErrorReporter;
  readonly handlers: ReadonlyMap<string, ZLinkRuntimePublishHandler>;
  readonly filters?: readonly ZLinkHandlerFilter[];
  readonly unhandled?: ZLinkUnhandledDispatchOptions;
  readonly metrics?: ZLinkRuntimeMetrics;
}

export interface ZLinkRuntimePublishHandler {
  handle(payload: unknown, context: ZLinkPublishMessageContext): Promise<void> | void;
}

export class ZLinkChannelPublishDispatcher {
  private readonly pipeline: ZLinkChannelDispatchPipeline;

  constructor(private readonly options: ZLinkChannelPublishDispatcherOptions) {
    this.pipeline = new ZLinkChannelDispatchPipeline({
      dispatchErrors: options.dispatchErrors,
      surface: ZLinkDispatchErrorSurface.Channel,
      channelName: options.channelName,
      filters: options.filters,
      unhandled: options.unhandled
    });
  }

  async dispatch(
    topicMessage: { readonly topic: string; readonly parts: readonly Message[] },
    signal?: AbortSignal,
    decodedHeader?: ZLinkChannelEnvelopeHeader
  ): Promise<void> {
    if (topicMessage.parts.length === 0 || topicMessage.parts[0].data().length === 0) {
      return;
    }
    let envelope: ReturnType<typeof decodeChannelEnvelope>;
    try {
      envelope = decodeChannelEnvelope(
        topicMessage.parts,
        decodedHeader,
        this.options.dispatchErrors.flow.flowCreationEnabled()
      );
    } catch (error) {
      // Malformed one-way envelope: drop with invalid_frame/drop.
      await this.pipeline.dispatchMalformed({
        info: inspectMalformedChannelHeader(
          topicMessage.parts,
          this.options.dispatchErrors.flow.flowCreationEnabled()
        ),
        error,
        transportRequest: false
      });
      return;
    }
    if (envelope.header.kind !== ZLinkChannelMessageKind.Publish) {
      return;
    }
    const packetName = envelope.packetName;
    if (packetName === undefined) {
      throw new ZLinkConfigurationException('Fanout publish message is missing packetName.');
    }
    const publishTopic = envelope.header.topic ?? topicMessage.topic;
    const publishSource = envelope.header.source ?? undefined;
    const publishCorr = envelope.header.correlationId ?? undefined;
    const fields: ZLinkChannelDispatchFields = {
      messageKind: ZLinkDispatchMessageKind.Publish,
      packetName,
      topic: publishTopic,
      sourceRid: publishSource,
      correlationId: publishCorr,
      flowId: envelope.header.flowId,
      flowOrigin: envelope.header.flowOrigin
    };
    const handler = this.options.handlers.get(packetName);
    const context: ZLinkPublishMessageContext = {
      meshName: this.options.meshName,
      channelName: this.options.channelName,
      packetName,
      contentType: envelope.header.contentType,
      topic: publishTopic,
      source: publishSource,
      metadata: zlinkMessageMetadata(envelope.header.metadata),
      correlationId: publishCorr
    };
    await this.pipeline.dispatchOneWay({
      fields,
      envelope,
      codecs: this.options.codecs,
      handler,
      context,
      signal
    });
  }
}

export interface ZLinkRoutePacketDispatcherOptions {
  readonly routerChannelId: string;
  readonly codecs?: ZLinkChannelEnvelopeCodecRegistry;
  readonly dispatchErrors: ZLinkDispatchErrorReporter;
  readonly handlers: readonly ZLinkRouteHandlerRegistration[];
  readonly filters?: readonly ZLinkHandlerFilter[];
  readonly unhandled?: ZLinkUnhandledDispatchOptions;
  readonly spotRouteBridge?: ZLinkBackendSpotRouteBridge;
}

export function collectRouteChannelHandlers(routeChannel: ZLinkRouteChannelOptions): ZLinkRouteHandlerRegistration[] {
  return [
    ...(routeChannel.handlers ?? []),
    ...(routeChannel.sendHandlers ?? []).map((handler): ZLinkRouteHandlerRegistration => ({
      kind: 'send',
      packetName: handler.packetName,
      handler: handler.handler
    })),
    ...(routeChannel.requestHandlers ?? []).map((handler): ZLinkRouteHandlerRegistration => ({
      kind: 'request',
      packetName: handler.packetName,
      handler: handler.handler
    }))
  ];
}

export class ZLinkRoutePacketDispatcher {
  private readonly sendHandlers = new Map<string, ZLinkRouteRuntimeSendHandler>();
  private readonly requestHandlers = new Map<string, ZLinkRouteRuntimeRequestHandler>();
  private readonly codecs?: ZLinkChannelEnvelopeCodecRegistry;
  private readonly pipeline: ZLinkChannelDispatchPipeline;

  constructor(options: ZLinkRoutePacketDispatcherOptions) {
    this.routerChannelId = options.routerChannelId;
    this.codecs = options.codecs;
    this.dispatchErrors = options.dispatchErrors;
    this.pipeline = new ZLinkChannelDispatchPipeline({
      dispatchErrors: options.dispatchErrors,
      surface: ZLinkDispatchErrorSurface.RouteMeshChannel,
      channelName: options.routerChannelId,
      filters: options.filters,
      unhandled: options.unhandled
    });
    this.spotRouteBridge = options.spotRouteBridge;
    for (const handler of options.handlers) {
      const target = handler.kind === 'send' ? this.sendHandlers : this.requestHandlers;
      if (target.has(handler.packetName)) {
        throw new ZLinkConfigurationException(`Duplicate routed handler '${options.routerChannelId}:${handler.kind}:${handler.packetName}'.`);
      }
      target.set(handler.packetName, handler.handler as never);
    }
  }

  private readonly routerChannelId: string;
  private readonly spotRouteBridge?: ZLinkBackendSpotRouteBridge;
  private readonly dispatchErrors: ZLinkDispatchErrorReporter;

  /** Spec 27 §4: with tracing Off, inbound flow fields are neither validated nor carried forward. */
  flowEnabled(): boolean {
    return this.dispatchErrors.flow.flowCreationEnabled();
  }

  dispatchInfrastructure(received: {
    parts: readonly Message[];
    routingId: unknown;
    spotId?: unknown;
    requestSeq: bigint | null;
    send?: () => ZLinkMultipartOperation<ZLinkMultipartSubmitOperation>;
  }): boolean {
    const channelHeader = tryDecodeChannelHeader(received.parts, this.flowEnabled());
    if (
      this.spotRouteBridge !== undefined
      && channelHeader === undefined
      && this.spotRouteBridge.handleRouterReceived(
        this.routerChannelId,
        received.routingId as RoutingId,
        received.requestSeq ?? 0n,
        received.parts
      )
    ) {
      return true;
    }
    if (received.spotId !== null && received.spotId !== undefined) {
      if (received.send === undefined) {
        throw new ZLinkConfigurationException('Routed SPOT packet is missing a local SPOT delivery context.');
      }
      appendParts(received.send(), received.parts).submit();
      return true;
    }
    return false;
  }

  async dispatch(received: {
    parts: readonly Message[];
    routingId: unknown;
    spotId?: unknown;
    requestSeq: bigint | null;
    send?: () => ZLinkMultipartOperation<ZLinkMultipartSubmitOperation>;
  }, router: {
    reply(routingId: unknown, requestSeq: bigint): ZLinkMultipartReplyOperation;
  }, signal?: AbortSignal, decodedHeader?: ZLinkChannelEnvelopeHeader, infrastructureChecked = false): Promise<boolean | void> {
    const channelHeader = decodedHeader ?? tryDecodeChannelHeader(received.parts, this.flowEnabled());
    if (!infrastructureChecked && this.dispatchInfrastructure(received)) {
      return true;
    }
    if (received.parts.length === 0 || received.parts[0].data().length === 0) {
      return;
    }
    let envelope: ReturnType<typeof decodeChannelEnvelope>;
    try {
      envelope = decodeChannelEnvelope(received.parts, channelHeader, this.flowEnabled());
    } catch (error) {
      const requestSeq = received.requestSeq;
      const info = inspectMalformedChannelHeader(received.parts, this.flowEnabled());
      await this.pipeline.dispatchMalformed({
        info,
        error,
        transportRequest: requestSeq !== null,
        sourceRid: String(received.routingId),
        writeProtocolError: requestSeq === null
          ? undefined
          : protocolError => this.submitReply(
              appendParts(
                router.reply(received.routingId, requestSeq),
                encodeChannelErrorReplyParts(
                  malformedProtocolErrorRequestHeader(this.routerChannelId, info),
                  protocolError
                )
              )
            )
      });
      return;
    }
    const packetName = envelope.packetName;
    if (packetName === undefined) {
      throw new ZLinkConfigurationException('Route packet is missing packetName.');
    }

    const routeCorr = envelope.header.correlationId ?? undefined;
    const routeSource = String(received.routingId);
    if (envelope.header.kind === ZLinkChannelMessageKind.Command) {
      const fields: ZLinkChannelDispatchFields = {
        messageKind: ZLinkDispatchMessageKind.Send,
        packetName,
        sourceRid: routeSource,
        correlationId: routeCorr,
        flowId: envelope.header.flowId,
        flowOrigin: envelope.header.flowOrigin
      };
      const handler = this.sendHandlers.get(packetName);
      await this.pipeline.dispatchOneWay({
        fields,
        envelope,
        codecs: codecsForFrameworkPacket(packetName, this.codecs),
        handler,
        context: this.createRouteContext(envelope, received.routingId),
        signal
      });
      return;
    }

    if (envelope.header.kind !== ZLinkChannelMessageKind.Request) {
      return;
    }

    if (received.requestSeq === null) {
      this.pipeline.dropMissingReplyPath({
        messageKind: ZLinkDispatchMessageKind.Request,
        packetName,
        sourceRid: routeSource,
        correlationId: routeCorr,
        flowId: envelope.header.flowId,
        flowOrigin: envelope.header.flowOrigin
      });
      return;
    }

    const requestFields: ZLinkChannelDispatchFields = {
      messageKind: ZLinkDispatchMessageKind.Request,
      packetName,
      sourceRid: routeSource,
      correlationId: routeCorr,
      flowId: envelope.header.flowId,
      flowOrigin: envelope.header.flowOrigin
    };
    const handler = this.requestHandlers.get(packetName);
    const requestSeq = received.requestSeq;
    const codecs = codecsForFrameworkPacket(packetName, this.codecs);
    await this.pipeline.dispatchRequest({
      fields: requestFields,
      envelope,
      codecs,
      handler,
      context: this.createRouteContext(envelope, received.routingId),
      signal,
      missingHandlerMessage: `No routed request handler is registered for '${this.routerChannelId}:${packetName}'.`,
      writeReply: reply => this.submitReply(
        appendParts(
          router.reply(received.routingId, requestSeq),
          encodeChannelReplyParts(envelope.header, reply, codecs)
        )
      ),
      writeError: error => this.submitReply(
        appendParts(
          router.reply(received.routingId, requestSeq),
          encodeChannelErrorReplyParts(envelope.header, error)
        )
      )
    });
  }

  async dispatchMesh(record: ReceiveRecord, signal?: AbortSignal): Promise<void> {
    if (record.parts.length === 0 || record.parts[0].data().length === 0) {
      return;
    }
    let envelope: ReturnType<typeof decodeChannelEnvelope>;
    try {
      envelope = decodeChannelEnvelope(record.parts, undefined, this.flowEnabled());
    } catch (error) {
      const info = inspectMalformedChannelHeader(record.parts, this.flowEnabled());
      await this.pipeline.dispatchMalformed({
        info,
        error,
        transportRequest: false,
        sourceRid: record.sourceNodeRid == null ? undefined : String(record.sourceNodeRid),
        writeProtocolError: async (protocolError) => {
          requireMeshReplyAccepted(record.reply(encodeChannelErrorReplyParts(
            malformedProtocolErrorRequestHeader(this.routerChannelId, info),
            protocolError
          )));
        }
      });
      return;
    }
    const packetName = envelope.packetName;
    if (packetName === undefined) {
      throw new ZLinkConfigurationException('Route packet is missing packetName.');
    }
    const sourceNodeRid = String(record.sourceNodeRid ?? '');
    const correlationId = envelope.header.correlationId ?? undefined;
    if (envelope.header.kind === ZLinkChannelMessageKind.Command) {
      await this.dispatchMeshCommand(envelope, packetName, sourceNodeRid, record.sourceNodeRid, signal);
      return;
    }
    if (envelope.header.kind !== ZLinkChannelMessageKind.Request) {
      return;
    }
    const fields: ZLinkChannelDispatchFields = {
      messageKind: ZLinkDispatchMessageKind.Request,
      packetName,
      sourceRid: sourceNodeRid,
      correlationId,
      flowId: envelope.header.flowId,
      flowOrigin: envelope.header.flowOrigin
    };
    const codecs = codecsForFrameworkPacket(packetName, this.codecs);
    await this.pipeline.dispatchRequest({
      fields,
      envelope,
      codecs,
      handler: this.requestHandlers.get(packetName),
      context: this.createRouteContext(envelope, record.sourceNodeRid),
      signal,
      missingHandlerMessage: `No routed request handler is registered for '${this.routerChannelId}:${packetName}'.`,
      writeReply: async (reply) => {
        requireMeshReplyAccepted(record.reply(encodeChannelReplyParts(envelope.header, reply, codecs)));
      },
      writeError: async (error) => {
        requireMeshReplyAccepted(record.reply(encodeChannelErrorReplyParts(envelope.header, error)));
      }
    });
  }

  async dispatchLocalCommand(
    parts: readonly Message[],
    sourceNodeRid: RoutingId,
    signal?: AbortSignal
  ): Promise<void> {
    if (parts.length === 0 || parts[0].data().length === 0) return;
    let envelope: ReturnType<typeof decodeChannelEnvelope>;
    try {
      envelope = decodeChannelEnvelope(parts, undefined, this.flowEnabled());
    } catch (error) {
      // Malformed one-way envelope: drop with invalid_frame/drop.
      await this.pipeline.dispatchMalformed({
        info: inspectMalformedChannelHeader(parts, this.flowEnabled()),
        error,
        transportRequest: false,
        sourceRid: String(sourceNodeRid)
      });
      return;
    }
    if (envelope.header.kind !== ZLinkChannelMessageKind.Command) {
      throw new ZLinkConfigurationException('Local node-direct dispatch requires a command packet.');
    }
    const packetName = envelope.packetName;
    if (packetName === undefined) {
      throw new ZLinkConfigurationException('Route packet is missing packetName.');
    }
    await this.dispatchMeshCommand(envelope, packetName, String(sourceNodeRid), sourceNodeRid, signal);
  }

  private dispatchMeshCommand(
    envelope: ReturnType<typeof decodeChannelEnvelope>,
    packetName: string,
    sourceRid: string,
    sourceNodeRid: unknown,
    signal?: AbortSignal
  ): Promise<void> {
    return this.pipeline.dispatchOneWay({
      fields: {
        messageKind: ZLinkDispatchMessageKind.Send,
        packetName,
        sourceRid,
        correlationId: envelope.header.correlationId ?? undefined,
        flowId: envelope.header.flowId,
        flowOrigin: envelope.header.flowOrigin
      },
      envelope,
      codecs: codecsForFrameworkPacket(packetName, this.codecs),
      handler: this.sendHandlers.get(packetName),
      context: this.createRouteContext(envelope, sourceNodeRid),
      signal
    });
  }

  private submitReply(operation: ZLinkMultipartReplyOperation): Promise<void> {
    if (operation.submit() === false) {
      throw new ZLinkConfigurationException('Route reply was not accepted by Core.');
    }
    return Promise.resolve();
  }

  private createRouteContext(
    envelope: ReturnType<typeof decodeChannelEnvelope>,
    sourceRid: unknown
  ): ZLinkRouteMessageContext {
    const sourceNodeRid = String(sourceRid ?? '');
    return {
      meshName: this.routerChannelId,
      packetName: envelope.packetName!,
      contentType: envelope.header.contentType,
      sourceNodeRid,
      metadata: zlinkMessageMetadata(envelope.header.metadata),
      correlationId: envelope.header.correlationId ?? undefined
    };
  }
}
