import {
  mapZLinkRequestReply,
  identityRequestReply,
  passthroughRequestReplyObserver,
  type ZLinkRequestReplyObserver
} from '../messaging/request-result';
import { ZLinkFrameworkException } from '../../contracts';
import type { Message } from '../../contracts/Common/Message';
import { DEFAULT_REQUEST_TIMEOUT_MS } from '../../contracts/Configuration/Registration';
import { ZLinkSpotKind } from '../../contracts/Spots';
import { throwIfAborted } from '../abort';
import type { ZLinkBackendMeshNode, ZLinkBackendSpot } from '../backend/contracts';
import {
  closeMeshCompletion,
  type ZLinkMeshCompletion,
  type ZLinkMeshCompletionTable
} from '../backend/mesh-completion-table';
import { ZLinkBufferMessage } from '../backend/runtime-message';
import {
  RequestResult,
  ZLINK_BACKEND_SEND_DONT_WAIT,
  isZLinkBackendResultError,
  type ZLinkBackendMessageLike as MessageLike
} from '../backend/runtime-values';
import { ZLinkConfigurationException } from '../configuration';
import type { ZLinkRuntimeMetrics } from '../diagnostics';
import { runWithOutboundFlow } from '../diagnostics/flow-context';
import { flowIfEnabled, type ZLinkMessageFlowTracer } from '../diagnostics';
import {
  ZLinkDispatchErrorSurface,
  ZLinkDispatchMessageKind,
  ZLinkRuntimeMessageFlowOutcome as ZLinkMessageFlowOutcome,
  type ZLinkRuntimeMessageFlowResult
} from '../../contracts/Dispatch/ZLinkDispatchOptions';
import { isRemoteChannelErrorReply, requestTerminalResult } from './channel-outbound-operations';
import { ServiceStaleGenerationError } from '../foundation/service-stateful-registry';
import type {
  ServiceDirectSpotRouteFence,
  ServiceInstanceRouteFence
} from '../foundation/service-stateful-wire-codec';
import {
  type ZLinkFrameworkInternalErrorKind as ZLinkFrameworkInternalErrorKindType,
  ZLinkFrameworkInternalErrorKind,
  createInternalFrameworkException,
  internalFrameworkErrorKind,
  internalFrameworkErrorKindFromWireReply,
  isCanonicalWireReplyTerminal,
  requestResultToPublicErrorKind
} from '../framework-errors-internal';

import {
  classifySubmitResult,
  submitToRequestResult,
  ZLinkSubmitStatus,
  type ZLinkSubmitResult
} from '../messaging/submission-result';
import { routingIdsEqual, toBackendRoutingId } from '../routing-id';
import type { ZLinkSpotRouteTarget } from '../spots/spot-routing-internal';
import type {
  ZLinkSpotRoutedSendOptions,
  ZLinkSpotRoutedRequestOptions
} from '../spots/spot-outbound';
import {
  ZLinkChannelMessageKind,
  decodeChannelReply,
  encodeChannelEnvelopeParts,
  newChannelCorrelationId,
  type ZLinkChannelEnvelopeCodecRegistry
} from './channel-envelope';

const NON_BLOCKING_SEND_OPTIONS = Object.freeze({ flags: ZLINK_BACKEND_SEND_DONT_WAIT });

export interface ZLinkChannelClientTransport {
  send(
    channelName: string,
    packetName: string | undefined,
    message: unknown,
    metadata?: ReadonlyMap<string, string>
  ): void | ZLinkSubmitResult | Promise<void | ZLinkSubmitResult>;
  request<TReply>(
    channelName: string,
    packetName: string | undefined,
    request: unknown,
    timeoutMs: number | undefined,
    signal?: AbortSignal,
    metadata?: ReadonlyMap<string, string>,
    onAdmitted?: () => void,
    replyObserver?: ZLinkRequestReplyObserver
  ): Promise<TReply>;
  tryPublish?(
    channelName: string,
    topic: string,
    packetName: string | undefined,
    event: unknown,
    metadata?: ReadonlyMap<string, string>
  ): ZLinkSubmitResult;
  publish(
    channelName: string,
    topic: string,
    packetName: string | undefined,
    event: unknown,
    signal?: AbortSignal,
    metadata?: ReadonlyMap<string, string>
  ): ZLinkSubmitResult | Promise<ZLinkSubmitResult>;
}

export type ZLinkChannelClientTransportSource =
  ZLinkChannelClientTransport | (() => ZLinkChannelClientTransport | undefined);

export interface ZLinkSpotPublisherClientTransport {
  tryPublish(
    meshName: string,
    channelName: string,
    topic: string,
    packetName: string | undefined,
    event: unknown,
    metadata?: ReadonlyMap<string, string>
  ): ZLinkSubmitResult;
  publish(
    meshName: string,
    channelName: string,
    topic: string,
    packetName: string | undefined,
    event: unknown,
    signal?: AbortSignal,
    metadata?: ReadonlyMap<string, string>
  ): ZLinkSubmitResult | Promise<ZLinkSubmitResult>;
}

export interface ZLinkRouteClientTransport {
  submit(
    routerChannelId: string,
    targetNodeRid: string,
    packetName: string | undefined,
    message: unknown,
    metadata?: ReadonlyMap<string, string>
  ): void | ZLinkSubmitResult | Promise<void | ZLinkSubmitResult>;
  request<TReply>(
    routerChannelId: string,
    targetNodeRid: string,
    packetName: string | undefined,
    request: unknown,
    timeoutMs: number | undefined,
    signal?: AbortSignal,
    metadata?: ReadonlyMap<string, string>,
    onAdmitted?: () => void,
    replyObserver?: ZLinkRequestReplyObserver
  ): Promise<TReply>;
  submitToChannel(
    meshName: string,
    channelName: string,
    packetName: string | undefined,
    message: unknown,
    metadata?: ReadonlyMap<string, string>
  ): void | ZLinkSubmitResult | Promise<void | ZLinkSubmitResult>;
  requestToChannel<TReply>(
    meshName: string,
    channelName: string,
    packetName: string | undefined,
    request: unknown,
    timeoutMs: number | undefined,
    signal?: AbortSignal,
    metadata?: ReadonlyMap<string, string>,
    onAdmitted?: () => void,
    replyObserver?: ZLinkRequestReplyObserver
  ): Promise<TReply>;
  sendToSpot?(
    spotRouteTarget: ZLinkSpotRouteTarget,
    message: unknown,
    options: {
      readonly packetName?: string;
      readonly metadata?: ReadonlyMap<string, string>;
    }
  ): Promise<ZLinkSubmitResult>;
  requestToSpot?<TReply = unknown>(
    spotRouteTarget: ZLinkSpotRouteTarget,
    request: unknown,
    options: {
      readonly packetName?: string;
      readonly timeoutMs?: number;
      readonly signal?: AbortSignal;
      readonly onAdmitted?: () => void;
      readonly metadata?: ReadonlyMap<string, string>;
    }
  ): Promise<TReply>;
}

interface ZLinkChannelTransportRuntime {
  send(
    channelName: string,
    packetName: string | undefined,
    message: unknown,
    metadata?: ReadonlyMap<string, string>
  ): Promise<ZLinkSubmitResult>;
  request<TReply>(
    channelName: string,
    packetName: string | undefined,
    request: unknown,
    timeoutMs: number | undefined,
    signal?: AbortSignal,
    metadata?: ReadonlyMap<string, string>,
    onAdmitted?: () => void,
    replyObserver?: ZLinkRequestReplyObserver
  ): Promise<TReply>;
  tryPublish(
    channelName: string,
    topic: string,
    packetName: string | undefined,
    event: unknown,
    metadata?: ReadonlyMap<string, string>
  ): ZLinkSubmitResult;
  publish(
    channelName: string,
    topic: string,
    packetName: string | undefined,
    event: unknown,
    signal?: AbortSignal,
    metadata?: ReadonlyMap<string, string>
  ): Promise<ZLinkSubmitResult>;
  canRouteChannel(routerChannelId: string): boolean;
  canRoutePacketChannel(routerChannelId: string): boolean;
  routeSubmit(
    routerChannelId: string,
    targetNodeRid: string,
    packetName: string | undefined,
    message: unknown,
    metadata?: ReadonlyMap<string, string>
  ): Promise<ZLinkSubmitResult>;
  routeRequest<TReply>(
    routerChannelId: string,
    targetNodeRid: string,
    packetName: string | undefined,
    request: unknown,
    timeoutMs: number | undefined,
    signal?: AbortSignal,
    metadata?: ReadonlyMap<string, string>,
    onAdmitted?: () => void
  ): Promise<TReply>;
  routeSendToSpot(
    spotRouteTarget: ZLinkSpotRouteTarget,
    packetName: string | undefined,
    message: unknown,
    metadata?: ReadonlyMap<string, string>
  ): Promise<void>;
  routeSendFromSpotToSpot(
    sourceSpot: ZLinkBackendSpot,
    spotRouteTarget: ZLinkSpotRouteTarget,
    packetName: string | undefined,
    message: unknown,
    metadata?: ReadonlyMap<string, string>
  ): Promise<void>;
  routeRequestToSpot<TReply>(
    spotRouteTarget: ZLinkSpotRouteTarget,
    packetName: string | undefined,
    request: unknown,
    timeoutMs: number | undefined,
    signal?: AbortSignal,
    metadata?: ReadonlyMap<string, string>,
    onAdmitted?: () => void
  ): Promise<TReply>;
  routeRequestFromSpotToSpot<TReply>(
    sourceSpot: ZLinkBackendSpot,
    spotRouteTarget: ZLinkSpotRouteTarget,
    packetName: string | undefined,
    request: unknown,
    timeoutMs: number | undefined,
    signal?: AbortSignal,
    onAdmitted?: () => void
  ): Promise<TReply>;
  routeRequestRawFromSpotToSpot(
    sourceSpot: ZLinkBackendSpot,
    spotRouteTarget: ZLinkSpotRouteTarget,
    request: Message,
    timeoutMs: number | undefined,
    signal?: AbortSignal,
    onAdmitted?: () => void
  ): Promise<readonly Message[]>;
  routeRequestRawToSpot(
    spotRouteTarget: ZLinkSpotRouteTarget,
    request: Message,
    timeoutMs: number | undefined,
    signal?: AbortSignal,
    onAdmitted?: () => void
  ): Promise<readonly Message[]>;
}

export class ZLinkRuntimeRouteTransport implements ZLinkRouteClientTransport {
  constructor(
    private readonly manager: () => ZLinkChannelTransportRuntime | undefined,
    private readonly routeChannelPredicate:
      ((routerChannelId: string) => boolean) | undefined = undefined,
    private readonly meshRuntime:
      | (() =>
          | {
              readonly meshNode?: (meshName: string) => ZLinkBackendMeshNode | undefined;
              readonly meshCompletionTable: (
                meshName: string
              ) => ZLinkMeshCompletionTable | undefined;
            }
          | undefined)
      | undefined = undefined,
    private readonly codecs?: ZLinkChannelEnvelopeCodecRegistry,
    private readonly manualNodeTarget?: (
      meshName: string,
      targetNodeRid: string
    ) => boolean | undefined,
    private readonly localNodeSubmit?: (
      meshName: string,
      sourceNodeRid: string,
      parts: readonly MessageLike[]
    ) => ZLinkSubmitResult,
    private readonly metrics?: ZLinkRuntimeMetrics,
    private readonly flowCreationEnabled: () => boolean = () => true,
    private readonly outboundFlow: () => ZLinkMessageFlowTracer | undefined = () => undefined
  ) {}

  canRouteChannel(routerChannelId: string): boolean {
    const manager = this.manager();
    if (manager !== undefined) {
      return manager.canRouteChannel(routerChannelId);
    }
    return this.routeChannelPredicate?.(routerChannelId) ?? false;
  }

  canRoutePacketChannel(routerChannelId: string): boolean {
    return (
      this.manager()?.canRoutePacketChannel(routerChannelId) ??
      this.routeChannelPredicate?.(routerChannelId) ??
      false
    );
  }

  submit(
    meshName: string,
    targetNodeRid: string,
    packetName: string | undefined,
    message: unknown,
    metadata?: ReadonlyMap<string, string>
  ): ZLinkSubmitResult | Promise<ZLinkSubmitResult> {
    // Call-scoped flow (spec 27 §4): the envelope flow pair lives only for
    // the duration of this outbound call, never in the caller's context.
    if (!this.flowCreationEnabled())
      return this.submitNode(meshName, targetNodeRid, packetName, message, metadata, false);
    return runWithOutboundFlow(true, () =>
      this.submitNode(meshName, targetNodeRid, packetName, message, metadata, false)
    );
  }

  submitInfrastructure(
    meshName: string,
    targetNodeRid: string,
    packetName: string | undefined,
    message: unknown,
    metadata?: ReadonlyMap<string, string>
  ): ZLinkSubmitResult | Promise<ZLinkSubmitResult> {
    if (!this.flowCreationEnabled())
      return this.submitNode(meshName, targetNodeRid, packetName, message, metadata, true);
    return runWithOutboundFlow(true, () =>
      this.submitNode(meshName, targetNodeRid, packetName, message, metadata, true)
    );
  }

  private submitNode(
    meshName: string,
    targetNodeRid: string,
    packetName: string | undefined,
    message: unknown,
    metadata: ReadonlyMap<string, string> | undefined,
    allowObjectClientTarget: boolean
  ): ZLinkSubmitResult | Promise<ZLinkSubmitResult> {
    const node = this.meshNode(meshName);
    if (node === undefined) {
      return this.requireManager().routeSubmit(
        meshName,
        targetNodeRid,
        packetName,
        message,
        metadata
      );
    }
    if (
      !allowObjectClientTarget &&
      node.isObjectClientNodeDirectTarget?.(toBackendRoutingId(targetNodeRid)) === true
    ) {
      return { status: ZLinkSubmitStatus.TargetNotFound };
    }
    const parts = this.encodeMessage(
      ZLinkChannelMessageKind.Command,
      meshName,
      packetName,
      message,
      undefined,
      metadata
    );
    const sourceNodeRid =
      node.routingId ?? (node.status().routingId as unknown as import('../../contracts').RoutingId);
    if (routingIdsEqual(sourceNodeRid, targetNodeRid)) {
      return this.submitLocalNode(meshName, String(sourceNodeRid), parts);
    }
    if (
      this.manualNodeTarget?.(meshName, targetNodeRid) === false &&
      !this.isKnownBackendPeer(node, targetNodeRid)
    ) {
      return { status: ZLinkSubmitStatus.TargetNotFound };
    }
    try {
      const submitted = node.sendToNode(
        toBackendRoutingId(targetNodeRid),
        parts,
        NON_BLOCKING_SEND_OPTIONS
      );
      return submitted instanceof Promise
        ? submitted.then(mapMeshSubmitResult, (error) => {
            throw mapMeshSubmissionError(
              error,
              `MeshNode '${meshName}' send to node '${targetNodeRid}'`
            );
          })
        : mapMeshSubmitResult(submitted);
    } catch (error) {
      throw mapMeshSubmissionError(error, `MeshNode '${meshName}' send to node '${targetNodeRid}'`);
    }
  }

  request<TReply>(
    meshName: string,
    targetNodeRid: string,
    packetName: string | undefined,
    request: unknown,
    timeoutMs: number | undefined,
    signal?: AbortSignal,
    metadata?: ReadonlyMap<string, string>,
    onAdmitted?: () => void,
    replyObserver: ZLinkRequestReplyObserver = passthroughRequestReplyObserver
  ): Promise<TReply> {
    if (!this.flowCreationEnabled())
      return this.requestScoped<TReply>(
        meshName,
        targetNodeRid,
        packetName,
        request,
        timeoutMs,
        signal,
        metadata,
        onAdmitted,
        replyObserver
      );
    return runWithOutboundFlow(true, () =>
      this.requestScoped<TReply>(
        meshName,
        targetNodeRid,
        packetName,
        request,
        timeoutMs,
        signal,
        metadata,
        onAdmitted,
        replyObserver
      )
    );
  }

  private requestScoped<TReply>(
    meshName: string,
    targetNodeRid: string,
    packetName: string | undefined,
    request: unknown,
    timeoutMs: number | undefined,
    signal: AbortSignal | undefined,
    metadata: ReadonlyMap<string, string> | undefined,
    onAdmitted: (() => void) | undefined,
    replyObserver: ZLinkRequestReplyObserver
  ): Promise<TReply> {
    let node: ZLinkBackendMeshNode | undefined;
    try {
      node = this.meshNode(meshName);
      if (node === undefined) {
        return mapZLinkRequestReply(
          this.requireManager().routeRequest<TReply>(
            meshName,
            targetNodeRid,
            packetName,
            request,
            timeoutMs,
            signal,
            metadata,
            onAdmitted
          ),
          identityRequestReply,
          replyObserver
        );
      }
    } catch (error) {
      return mapZLinkRequestReply(Promise.reject(error), identityRequestReply, replyObserver);
    }
    const correlationId = newChannelCorrelationId();
    const traceFailure = (error: unknown): unknown => {
      if (!isRemoteChannelErrorReply(error)) {
        this.traceNodeRequest(
          ZLinkMessageFlowOutcome.ReplyReceived,
          requestTerminalResult(error, signal),
          meshName,
          targetNodeRid,
          packetName,
          correlationId,
          error
        );
      }
      return error;
    };
    try {
      throwIfAborted(signal);
      if (node.isObjectClientNodeDirectTarget?.(toBackendRoutingId(targetNodeRid)) === true) {
        throw createInternalFrameworkException(
          ZLinkFrameworkInternalErrorKind.RequestTargetNotFound,
          `MeshNode '${meshName}' target '${targetNodeRid}' is an Object Client.`
        );
      }
      const parts = this.encodeMessage(
        ZLinkChannelMessageKind.Request,
        meshName,
        packetName,
        request,
        timeoutMs ?? DEFAULT_REQUEST_TIMEOUT_MS,
        metadata,
        correlationId
      );
      const requestOptions = { flags: 1, onAdmitted, timeoutMs: requestTimeoutBudget(timeoutMs) };
      let pending: Promise<ZLinkMeshCompletion>;
      try {
        pending = this.completionTable(meshName).submit(() => {
          const operationId = node.requestToNode(
            toBackendRoutingId(targetNodeRid),
            parts,
            requestOptions
          );
          this.traceNodeRequest(
            ZLinkMessageFlowOutcome.Sent,
            undefined,
            meshName,
            targetNodeRid,
            packetName,
            correlationId
          );
          return operationId;
        }, signal);
      } catch (error) {
        pending = Promise.reject(error);
      }
      return mapZLinkRequestReply(
        pending,
        (completion) => {
          try {
            const reply = this.decodeMeshReply<TReply>(meshName, completion);
            this.traceNodeRequest(
              ZLinkMessageFlowOutcome.ReplyReceived,
              'succeeded',
              meshName,
              targetNodeRid,
              packetName,
              correlationId
            );
            return reply;
          } catch (error) {
            throw traceFailure(error);
          }
        },
        replyObserver,
        (error) =>
          traceFailure(
            mapMeshSubmissionError(
              error,
              `MeshNode '${meshName}' request to node '${targetNodeRid}'`
            )
          )
      );
    } catch (error) {
      return mapZLinkRequestReply(
        Promise.reject(traceFailure(error)),
        identityRequestReply,
        replyObserver
      );
    }
  }

  private traceNodeRequest(
    outcome: ZLinkMessageFlowOutcome,
    result: ZLinkRuntimeMessageFlowResult | undefined,
    meshName: string,
    targetRid: string,
    packetName: string | undefined,
    correlationId: string,
    error?: unknown
  ): void {
    if (!this.flowCreationEnabled()) return;
    flowIfEnabled(this.outboundFlow(), outcome, result)?.trace({
      outcome,
      result,
      surface: ZLinkDispatchErrorSurface.Node,
      messageKind: ZLinkDispatchMessageKind.Request,
      meshName,
      targetRid,
      packetName,
      correlationId,
      ...(error instanceof Error ? { errorType: error.name, errorMessage: error.message } : {})
    });
  }

  submitToChannel(
    meshName: string,
    channelName: string,
    packetName: string | undefined,
    message: unknown,
    metadata?: ReadonlyMap<string, string>
  ): Promise<ZLinkSubmitResult> {
    return runWithOutboundFlow(this.flowCreationEnabled(), () =>
      this.submitToChannelScoped(meshName, channelName, packetName, message, metadata)
    );
  }

  private async submitToChannelScoped(
    meshName: string,
    channelName: string,
    packetName: string | undefined,
    message: unknown,
    metadata?: ReadonlyMap<string, string>
  ): Promise<ZLinkSubmitResult> {
    const node = this.requireMeshNode(meshName);
    const parts = this.encodeMessage(
      ZLinkChannelMessageKind.Command,
      channelName,
      packetName,
      message,
      undefined,
      metadata
    );
    const operation = `MeshNode '${meshName}' send to channel '${channelName}'`;
    try {
      const result = mapMeshSubmitResult(
        await node.sendToChannel(channelName, parts, NON_BLOCKING_SEND_OPTIONS)
      );
      if (result.status === ZLinkSubmitStatus.TargetNotFound) {
        this.metrics?.recordChannelSelectionFailure(meshName, channelName, 'no_ready_target');
      }
      return result;
    } catch (error) {
      throw mapMeshSubmissionError(error, operation);
    }
  }

  requestToChannel<TReply>(
    meshName: string,
    channelName: string,
    packetName: string | undefined,
    request: unknown,
    timeoutMs: number | undefined,
    signal?: AbortSignal,
    metadata?: ReadonlyMap<string, string>,
    onAdmitted?: () => void,
    replyObserver: ZLinkRequestReplyObserver = passthroughRequestReplyObserver
  ): Promise<TReply> {
    if (!this.flowCreationEnabled())
      return this.requestToChannelScoped<TReply>(
        meshName,
        channelName,
        packetName,
        request,
        timeoutMs,
        signal,
        metadata,
        onAdmitted,
        replyObserver
      );
    return runWithOutboundFlow(true, () =>
      this.requestToChannelScoped<TReply>(
        meshName,
        channelName,
        packetName,
        request,
        timeoutMs,
        signal,
        metadata,
        onAdmitted,
        replyObserver
      )
    );
  }

  private requestToChannelScoped<TReply>(
    meshName: string,
    channelName: string,
    packetName: string | undefined,
    request: unknown,
    timeoutMs: number | undefined,
    signal: AbortSignal | undefined,
    metadata: ReadonlyMap<string, string> | undefined,
    onAdmitted: (() => void) | undefined,
    replyObserver: ZLinkRequestReplyObserver
  ): Promise<TReply> {
    try {
      throwIfAborted(signal);
      const node = this.requireMeshNode(meshName);
      const parts = this.encodeMessage(
        ZLinkChannelMessageKind.Request,
        channelName,
        packetName,
        request,
        timeoutMs ?? DEFAULT_REQUEST_TIMEOUT_MS,
        metadata
      );
      const metric = this.metrics?.startRequest(meshName, 'channel');
      const recordFailure = (error: unknown): unknown => {
        metric?.complete(requestMetricOutcome(error, signal));
        if (
          error instanceof ZLinkFrameworkException &&
          internalFrameworkErrorKind(error) ===
            ZLinkFrameworkInternalErrorKind.RequestTargetNotFound
        ) {
          this.metrics?.recordChannelSelectionFailure(meshName, channelName, 'no_ready_target');
        }
        return error;
      };
      const requestOptions = { flags: 1, timeoutMs: requestTimeoutBudget(timeoutMs), onAdmitted };
      let pending: Promise<ZLinkMeshCompletion>;
      try {
        pending = this.completionTable(meshName).submit(
          () => node.requestToChannel(channelName, parts, requestOptions),
          signal
        );
      } catch (error) {
        pending = Promise.reject(error);
      }
      return mapZLinkRequestReply(
        pending,
        (completion) => {
          try {
            const reply = this.decodeMeshReply<TReply>(meshName, completion);
            metric?.complete('completed');
            return reply;
          } catch (error) {
            throw recordFailure(error);
          }
        },
        replyObserver,
        (error) =>
          recordFailure(
            mapMeshSubmissionError(
              error,
              `MeshNode '${meshName}' request to channel '${channelName}'`
            )
          )
      );
    } catch (error) {
      return mapZLinkRequestReply(Promise.reject(error), identityRequestReply, replyObserver);
    }
  }

  sendToSpot(
    spotRouteTarget: ZLinkSpotRouteTarget,
    message: unknown,
    options: ZLinkSpotRoutedSendOptions
  ): Promise<ZLinkSubmitResult> {
    return runWithOutboundFlow(this.flowCreationEnabled(), () =>
      this.sendToSpotScoped(spotRouteTarget, message, options)
    );
  }

  private async sendToSpotScoped(
    spotRouteTarget: ZLinkSpotRouteTarget,
    message: unknown,
    options: ZLinkSpotRoutedSendOptions
  ): Promise<ZLinkSubmitResult> {
    const node = this.meshNode(spotRouteTarget.routerChannelId);
    if (node === undefined) {
      if (spotRouteTarget.spotKind === ZLinkSpotKind.Instance) {
        throw new ZLinkConfigurationException(
          'Instance Spot routes require the MeshNode command 39 transport.'
        );
      }
      await this.requireManager().routeSendToSpot(
        spotRouteTarget,
        options.packetName,
        message,
        options.metadata
      );
      return { status: ZLinkSubmitStatus.Submitted };
    }
    const operation = `MeshNode '${spotRouteTarget.routerChannelId}' send to Spot '${spotRouteTarget.spotId}'`;
    try {
      const encoded = this.encodeMessage(
        ZLinkChannelMessageKind.Command,
        spotRouteTarget.routerChannelId,
        options.packetName,
        message,
        undefined,
        options.metadata
      );
      if (spotRouteTarget.spotKind === ZLinkSpotKind.Instance) {
        return mapMeshSubmitResult(
          await node.sendToInstanceSpot(
            instanceSpotRouteFence(spotRouteTarget),
            encoded,
            undefined,
            options.metadata,
            options.instanceSpot
          )
        );
      }
      return mapMeshSubmitResult(
        await node
          .entrySpot()
          .sendToSpot(
            toBackendRoutingId(spotRouteTarget.targetNodeRid),
            toBackendRoutingId(spotRouteTarget.spotId),
            spotRouteTarget.targetSpotGeneration ?? 0n,
            encoded,
            {
              flags: 1,
              routeFence: directSpotRouteFence(spotRouteTarget),
              entrySpot: spotRouteTarget.spotKind === ZLinkSpotKind.Entry
            }
          )
      );
    } catch (error) {
      throw mapMeshSubmissionError(error, operation);
    }
  }

  async sendFromSpotToSpot(
    sourceSpot: ZLinkBackendSpot,
    spotRouteTarget: ZLinkSpotRouteTarget,
    message: unknown,
    options: {
      readonly packetName?: string;
      readonly metadata?: ReadonlyMap<string, string>;
    }
  ): Promise<ZLinkSubmitResult> {
    await this.requireManager().routeSendFromSpotToSpot(
      sourceSpot,
      spotRouteTarget,
      options.packetName,
      message,
      options.metadata
    );
    return { status: ZLinkSubmitStatus.Submitted };
  }

  requestToSpot<TReply = unknown>(
    spotRouteTarget: ZLinkSpotRouteTarget,
    request: unknown,
    options: ZLinkSpotRoutedRequestOptions
  ): Promise<TReply> {
    return runWithOutboundFlow(this.flowCreationEnabled(), () =>
      this.requestToSpotScoped<TReply>(spotRouteTarget, request, options)
    );
  }

  private async requestToSpotScoped<TReply = unknown>(
    spotRouteTarget: ZLinkSpotRouteTarget,
    request: unknown,
    options: ZLinkSpotRoutedRequestOptions
  ): Promise<TReply> {
    try {
      const meshName = spotRouteTarget.routerChannelId;
      const node = this.meshNode(meshName);
      if (node === undefined) {
        if (spotRouteTarget.spotKind === ZLinkSpotKind.Instance) {
          throw new ZLinkConfigurationException(
            'Instance Spot routes require the MeshNode command 39 transport.'
          );
        }
        return mapZLinkRequestReply(
          this.requireManager().routeRequestToSpot<TReply>(
            spotRouteTarget,
            options.packetName,
            request,
            options.timeoutMs,
            options.signal,
            options.metadata,
            options.onAdmitted
          ),
          identityRequestReply,
          options.replyObserver
        );
      }
      throwIfAborted(options.signal);
      let completionPromise: Promise<ZLinkMeshCompletion>;
      try {
        const encoded = this.encodeMessage(
          ZLinkChannelMessageKind.Request,
          meshName,
          options.packetName,
          request,
          options.timeoutMs,
          options.metadata
        );
        completionPromise = this.completionTable(meshName).submit(
          () =>
            spotRouteTarget.spotKind === ZLinkSpotKind.Instance
              ? node.requestInstanceSpot(
                  instanceSpotRouteFence(spotRouteTarget),
                  encoded,
                  options.timeoutMs,
                  undefined,
                  options.metadata,
                  options.instanceSpot,
                  options.onAdmitted
                )
              : node
                  .entrySpot()
                  .requestToSpot(
                    toBackendRoutingId(spotRouteTarget.targetNodeRid),
                    toBackendRoutingId(spotRouteTarget.spotId),
                    spotRouteTarget.targetSpotGeneration ?? 0n,
                    encoded,
                    {
                      flags: 1,
                      onAdmitted: options.onAdmitted,
                      timeoutMs: options.timeoutMs,
                      routeFence: directSpotRouteFence(spotRouteTarget),
                      entrySpot: spotRouteTarget.spotKind === ZLinkSpotKind.Entry
                    }
                  ),
          options.signal
        );
      } catch (error) {
        throw mapMeshSubmissionError(
          error,
          `MeshNode '${meshName}' request to Spot '${spotRouteTarget.spotId}'`
        );
      }
      return this.waitForMeshReply<TReply>(meshName, completionPromise, options.replyObserver);
    } catch (error) {
      (options.replyObserver ?? passthroughRequestReplyObserver).fail(error);
      throw error;
    }
  }

  requestFromSpotToSpot<TReply = unknown>(
    sourceSpot: ZLinkBackendSpot,
    spotRouteTarget: ZLinkSpotRouteTarget,
    request: unknown,
    options: {
      readonly packetName?: string;
      readonly timeoutMs?: number;
      readonly signal?: AbortSignal;
      readonly onAdmitted?: () => void;
      readonly replyObserver?: ZLinkRequestReplyObserver;
    }
  ): Promise<TReply> {
    let pending: Promise<TReply>;
    try {
      pending = this.requireManager().routeRequestFromSpotToSpot<TReply>(
        sourceSpot,
        spotRouteTarget,
        options.packetName,
        request,
        options.timeoutMs,
        options.signal,
        options.onAdmitted
      );
    } catch (error) {
      pending = Promise.reject(error);
    }
    return mapZLinkRequestReply(pending, identityRequestReply, options.replyObserver);
  }

  async requestRawFromSpotToSpot(
    sourceSpot: ZLinkBackendSpot,
    spotRouteTarget: ZLinkSpotRouteTarget,
    request: Message,
    options: {
      readonly timeoutMs?: number;
      readonly signal?: AbortSignal;
      readonly onAdmitted?: () => void;
    }
  ): Promise<readonly Message[]> {
    return this.requireManager().routeRequestRawFromSpotToSpot(
      sourceSpot,
      spotRouteTarget,
      request,
      options.timeoutMs,
      options.signal,
      options.onAdmitted
    );
  }

  requestRawToSpot(
    spotRouteTarget: ZLinkSpotRouteTarget,
    request: Message,
    options: {
      readonly timeoutMs?: number;
      readonly signal?: AbortSignal;
      readonly onAdmitted?: () => void;
    }
  ): Promise<readonly Message[]> {
    return runWithOutboundFlow(this.flowCreationEnabled(), () =>
      this.requestRawToSpotScoped(spotRouteTarget, request, options)
    );
  }

  private async requestRawToSpotScoped(
    spotRouteTarget: ZLinkSpotRouteTarget,
    request: Message,
    options: {
      readonly timeoutMs?: number;
      readonly signal?: AbortSignal;
      readonly onAdmitted?: () => void;
    }
  ): Promise<readonly Message[]> {
    const meshName = spotRouteTarget.routerChannelId;
    const node = this.meshNode(meshName);
    if (node !== undefined) {
      throwIfAborted(options.signal);
      const payload = JSON.parse(request.getString('utf8')) as Record<string, unknown>;
      const packetName = typeof payload.packetName === 'string' ? payload.packetName : undefined;
      const parts = this.encodeMessage(
        ZLinkChannelMessageKind.Request,
        meshName,
        packetName,
        payload,
        options.timeoutMs
      );
      let completionPromise: Promise<ZLinkMeshCompletion>;
      try {
        completionPromise = this.completionTable(meshName).submit(
          () =>
            node
              .entrySpot()
              .requestToSpot(
                toBackendRoutingId(spotRouteTarget.targetNodeRid),
                toBackendRoutingId(spotRouteTarget.spotId),
                spotRouteTarget.targetSpotGeneration ?? 0n,
                parts,
                {
                  flags: 1,
                  onAdmitted: options.onAdmitted,
                  timeoutMs: options.timeoutMs,
                  routeFence: directSpotRouteFence(spotRouteTarget),
                  entrySpot: spotRouteTarget.spotKind === ZLinkSpotKind.Entry
                }
              ),
          options.signal
        );
      } catch (error) {
        throw mapMeshSubmissionError(
          error,
          `MeshNode '${meshName}' raw request to Spot '${spotRouteTarget.spotId}'`
        );
      }
      const completion = await completionPromise;
      if (completion.terminalResult !== 0 || completion.failureErrno !== 0) {
        try {
          throw meshRequestFailure(meshName, completion.terminalResult, completion.failureErrno);
        } finally {
          closeMeshCompletion(completion);
        }
      }
      try {
        const reply = decodeChannelReply<unknown>(completion.parts, this.codecs);
        return [ZLinkBufferMessage.fromOwned(Buffer.from(JSON.stringify(reply)))];
      } finally {
        closeMeshCompletion(completion);
      }
    }
    return this.requireManager().routeRequestRawToSpot(
      spotRouteTarget,
      request,
      options.timeoutMs,
      options.signal,
      options.onAdmitted
    );
  }

  private requireManager(): ZLinkChannelTransportRuntime {
    const manager = this.manager();
    if (manager === undefined) {
      throw new ZLinkConfigurationException('Route channel runtime is not started.');
    }
    return manager;
  }

  private meshNode(meshName: string): ZLinkBackendMeshNode | undefined {
    return this.meshRuntime?.()?.meshNode?.(meshName);
  }

  private isKnownBackendPeer(node: ZLinkBackendMeshNode, targetNodeRid: string): boolean {
    return node
      .peers()
      .some(
        (peer) =>
          peer.routingId !== null &&
          routingIdsEqual(
            peer.routingId as unknown as import('../../contracts').RoutingId,
            targetNodeRid
          )
      );
  }

  private submitLocalNode(
    meshName: string,
    sourceNodeRid: string,
    parts: readonly MessageLike[]
  ): ZLinkSubmitResult {
    if (this.localNodeSubmit === undefined) {
      return { status: ZLinkSubmitStatus.TargetNotFound };
    }
    return this.localNodeSubmit(meshName, sourceNodeRid, parts);
  }

  private requireMeshNode(meshName: string): ZLinkBackendMeshNode {
    const node = this.meshNode(meshName);
    if (node === undefined) {
      throw new ZLinkConfigurationException(`MeshNode '${meshName}' runtime is not started.`);
    }
    return node;
  }

  private completionTable(meshName: string): ZLinkMeshCompletionTable {
    const table = this.meshRuntime?.()?.meshCompletionTable(meshName);
    if (table === undefined) {
      throw new ZLinkConfigurationException(
        `MeshNode '${meshName}' completion runtime is not started.`
      );
    }
    return table;
  }

  private encodeMessage(
    kind: ZLinkChannelMessageKind,
    channelName: string,
    packetName: string | undefined,
    message: unknown,
    timeoutMs?: number,
    metadata?: ReadonlyMap<string, string>,
    correlationId?: string
  ): readonly MessageLike[] {
    return encodeChannelEnvelopeParts(
      kind,
      channelName,
      packetName,
      message,
      timeoutMs,
      undefined,
      this.codecs,
      correlationId,
      this.flowCreationEnabled(),
      metadata
    );
  }

  private waitForMeshReply<TReply>(
    meshName: string,
    completionPromise: Promise<ZLinkMeshCompletion>,
    replyObserver: ZLinkRequestReplyObserver = passthroughRequestReplyObserver
  ): Promise<TReply> {
    return mapZLinkRequestReply(
      completionPromise,
      (completion) => this.decodeMeshReply<TReply>(meshName, completion),
      replyObserver
    );
  }

  private decodeMeshReply<TReply>(meshName: string, completion: ZLinkMeshCompletion): TReply {
    try {
      if (completion.terminalResult !== 0 || completion.failureErrno !== 0) {
        throw meshRequestFailure(meshName, completion.terminalResult, completion.failureErrno);
      }
      return decodeChannelReply<TReply>(completion.parts, this.codecs);
    } finally {
      closeMeshCompletion(completion);
    }
  }
}

function directSpotRouteFence(
  target: ZLinkSpotRouteTarget
): ServiceDirectSpotRouteFence | undefined {
  // Entry Spots are addressed by their owning MeshNode. They have no
  // Location Store authority row, so only User and Instance Spots carry the
  // exact Ready authority fence required by direct Spot submission.
  if (target.spotKind === ZLinkSpotKind.Entry) {
    return undefined;
  }
  if (
    target.targetSpotGeneration === undefined ||
    target.targetNodeGeneration === undefined ||
    target.authorityOwnerGeneration === undefined ||
    target.targetOwnerId === undefined ||
    target.ownerLeaseGeneration === undefined ||
    target.authorityStoreVersion === undefined
  ) {
    throw createInternalFrameworkException(
      ZLinkFrameworkInternalErrorKind.RouteNotConnected,
      `Spot '${String(target.spotId)}' has no complete Ready authority fence.`
    );
  }
  return {
    spot: {
      spotId: String(target.spotId),
      generation: target.targetSpotGeneration
    },
    targetNodeRid: String(target.targetNodeRid),
    targetNodeGeneration: target.targetNodeGeneration,
    authorityOwnerGeneration: target.authorityOwnerGeneration,
    ownerLeaseGeneration: target.ownerLeaseGeneration,
    storeVersion: target.authorityStoreVersion
  };
}

function instanceSpotRouteFence(target: ZLinkSpotRouteTarget): ServiceInstanceRouteFence {
  if (
    target.targetSpotGeneration === undefined ||
    target.targetNodeGeneration === undefined ||
    target.authorityOwnerGeneration === undefined ||
    target.targetOwnerId === undefined ||
    target.ownerLeaseGeneration === undefined ||
    target.authorityStoreVersion === undefined
  ) {
    throw createInternalFrameworkException(
      ZLinkFrameworkInternalErrorKind.SpotRouteNotFound,
      `Instance Spot '${String(target.spotId)}' has no complete Ready authority fence.`
    );
  }
  return {
    targetNodeRid: String(target.targetNodeRid),
    targetNodeGeneration: target.targetNodeGeneration,
    targetSpotId: String(target.spotId),
    objectGeneration: target.targetSpotGeneration,
    ownerId: target.targetOwnerId,
    authorityOwnerGeneration: target.authorityOwnerGeneration,
    leaseGeneration: target.ownerLeaseGeneration,
    storeVersion: target.authorityStoreVersion
  };
}

/**
 * The binding request timeout for one submission. The binding owns the
 * deadline after submit; the completion table slot only waits for it.
 */
function requestTimeoutBudget(timeoutMs: number | undefined): number {
  return Math.max(1, Math.ceil(timeoutMs ?? DEFAULT_REQUEST_TIMEOUT_MS));
}

function mapMeshSubmitResult(result: number): ZLinkSubmitResult {
  return classifySubmitResult(result, 'Mesh submission');
}

function mapMeshSubmissionError(error: unknown, operation: string): Error {
  if (error instanceof ZLinkFrameworkException) {
    return error;
  }
  // A remote authority route can advance its durable StoreVersion while the
  // object generation remains unchanged. The stateful runtime reports that
  // exact-fence mismatch as a raw ServiceStaleGenerationError before native
  // submission. Preserve the retryable route meaning so the address
  // transport can invalidate the Location Store cache and resolve again.
  if (error instanceof ServiceStaleGenerationError) {
    return createInternalFrameworkException(
      ZLinkFrameworkInternalErrorKind.SpotGenerationStale,
      `${operation} received a stale Spot authority fence: ${error.message}`,
      true,
      error
    );
  }
  if (isZLinkBackendResultError(error)) {
    const terminal =
      error.operation === 'request'
        ? error.result
        : submitToRequestResult(error.result, error.phase);
    if (terminal === RequestResult.NotFound) {
      return createInternalFrameworkException(
        ZLinkFrameworkInternalErrorKind.RequestTargetNotFound,
        `${operation} failed with result ${error.result}.`,
        false,
        error
      );
    }
    return new ZLinkFrameworkException(
      requestResultToPublicErrorKind(terminal),
      `${operation} failed with result ${error.result}.`,
      error
    );
  }
  return createInternalFrameworkException(
    ZLinkFrameworkInternalErrorKind.RouteNotConnected,
    `${operation} failed before native submission completed: ${
      error instanceof Error ? error.message : String(error)
    }`,
    true,
    error
  );
}

export function meshRequestFailure(
  meshName: string,
  result: number,
  nativeErrno: number
): ZLinkFrameworkException {
  const canonical = isCanonicalWireReplyTerminal(result, nativeErrno);
  const wireKind = canonical
    ? internalFrameworkErrorKindFromWireReply(result, nativeErrno)
    : undefined;
  const localNoMember = result === RequestResult.NotFound && nativeErrno === 0;
  const kind: ZLinkFrameworkInternalErrorKindType = localNoMember
    ? ZLinkFrameworkInternalErrorKind.RequestTargetNotFound
    : !canonical
      ? ZLinkFrameworkInternalErrorKind.RequestProtocolError
      : result === RequestResult.NotFound
        ? ZLinkFrameworkInternalErrorKind.RequestTargetNotFound
        : result === RequestResult.TimedOut
          ? ZLinkFrameworkInternalErrorKind.DeadlineExceeded
          : result === RequestResult.Terminated
            ? ZLinkFrameworkInternalErrorKind.RuntimeShutdown
            : result === RequestResult.Conflict || result === RequestResult.InternalError
              ? (wireKind ?? ZLinkFrameworkInternalErrorKind.RequestProtocolError)
              : result === RequestResult.NotConnected || result === RequestResult.Backpressured
                ? ZLinkFrameworkInternalErrorKind.RouteNotConnected
                : (wireKind ?? ZLinkFrameworkInternalErrorKind.RequestFailed);
  const error = createInternalFrameworkException(
    kind,
    `MeshNode '${meshName}' request failed with result ${result} and errno ${nativeErrno}.`,
    result === RequestResult.NotConnected || result === RequestResult.Backpressured
  );
  // An operation id was returned before this completion was observed. The
  // application envelope therefore crossed the native submission boundary,
  // including terminal NotFound replies; callers must not resubmit it through
  // a cold route.
  Object.defineProperty(error, 'physicalSubmission', {
    value: true,
    enumerable: false
  });
  return error;
}

function requestMetricOutcome(error: unknown, signal?: AbortSignal): string {
  if (error instanceof ZLinkFrameworkException) {
    if (internalFrameworkErrorKind(error) === ZLinkFrameworkInternalErrorKind.DeadlineExceeded)
      return 'timed_out';
    if (internalFrameworkErrorKind(error) === ZLinkFrameworkInternalErrorKind.RuntimeShutdown)
      return 'shutdown';
  }
  if (
    signal?.aborted === true &&
    (error === signal.reason ||
      (error instanceof Error && (error.cause === signal.reason || error.name === 'AbortError')))
  )
    return 'cancelled';
  return 'failed';
}
