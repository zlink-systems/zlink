import { RequestRuntime } from '../foundation/request-runtime';
import type { PendingOperation, OperationObserver } from '../foundation/operation-registry';
import { identityRequestReply, type ZLinkRequestResult } from '../messaging/request-result';
import { ZLinkFrameworkErrorKind, ZLinkFrameworkException } from '../../contracts';
import type { Message } from '../../contracts/Common/Message';
import { DEFAULT_REQUEST_TIMEOUT_MS } from '../../contracts/Configuration/Registration';
import { ZLinkSpotKind } from '../../contracts/Spots';
import { isAbortError, throwIfAborted } from '../abort';
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
  closeMessages,
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
    requestOperation?: PendingOperation<unknown>
  ): ZLinkRequestResult<TReply>;
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
    requestOperation?: PendingOperation<unknown>
  ): ZLinkRequestResult<TReply>;
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
    requestOperation?: PendingOperation<unknown>
  ): ZLinkRequestResult<TReply>;
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
      readonly requestOperation?: PendingOperation<unknown>;
      readonly signal?: AbortSignal;
      readonly onAdmitted?: () => void;
      readonly metadata?: ReadonlyMap<string, string>;
    }
  ): ZLinkRequestResult<TReply>;
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
    requestOperation?: PendingOperation<unknown>
  ): ZLinkRequestResult<TReply>;
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
    onAdmitted?: () => void,
    requestOperation?: PendingOperation<unknown>
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
    onAdmitted?: () => void,
    requestOperation?: PendingOperation<unknown>,
    destination?: OperationObserver<unknown, unknown>
  ): Promise<TReply>;
  routeRequestFromSpotToSpot<TReply>(
    sourceSpot: ZLinkBackendSpot,
    spotRouteTarget: ZLinkSpotRouteTarget,
    packetName: string | undefined,
    request: unknown,
    timeoutMs: number | undefined,
    signal?: AbortSignal,
    onAdmitted?: () => void,
    requestOperation?: PendingOperation<unknown>,
    destination?: OperationObserver<unknown, unknown>
  ): Promise<TReply>;
  routeRequestRawFromSpotToSpot(
    sourceSpot: ZLinkBackendSpot,
    spotRouteTarget: ZLinkSpotRouteTarget,
    request: Message,
    timeoutMs: number | undefined,
    signal?: AbortSignal,
    onAdmitted?: () => void,
    requestOperation?: PendingOperation<unknown>
  ): Promise<readonly Message[]>;
  routeRequestRawToSpot(
    spotRouteTarget: ZLinkSpotRouteTarget,
    request: Message,
    timeoutMs: number | undefined,
    signal?: AbortSignal,
    onAdmitted?: () => void,
    requestOperation?: PendingOperation<unknown>
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
    private readonly outboundFlow: () => ZLinkMessageFlowTracer | undefined = () => undefined,
    private readonly requests = new RequestRuntime()
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
    requestOperation?: PendingOperation<unknown>
  ): ZLinkRequestResult<TReply> {
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
        requestOperation
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
        requestOperation
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
    existing?: PendingOperation<unknown>
  ): ZLinkRequestResult<TReply> {
    const node = this.meshNode(meshName);
    if (node === undefined) {
      return this.requests.prepare<TReply>(
        timeoutMs ?? DEFAULT_REQUEST_TIMEOUT_MS,
        signal,
        (operation) => {
          this.requireManager().routeRequest<TReply>(
            meshName,
            targetNodeRid,
            packetName,
            request,
            timeoutMs,
            signal,
            metadata,
            onAdmitted,
            operation
          );
        },
        (value) => value as TReply,
        identityRequestReply,
        existing
      );
    }
    const correlationId = newChannelCorrelationId();
    const result = this.requests.prepare<TReply>(
      timeoutMs ?? DEFAULT_REQUEST_TIMEOUT_MS,
      signal,
      (requestOperation, receiver) => {
        requestOperation.mapCaller(
          (value) => {
            this.traceNodeRequest(
              ZLinkMessageFlowOutcome.ReplyReceived,
              'succeeded',
              meshName,
              targetNodeRid,
              packetName,
              correlationId
            );
            return value;
          },
          (error) => {
            if (!isRemoteChannelErrorReply(error))
              this.traceNodeRequest(
                ZLinkMessageFlowOutcome.ReplyReceived,
                requestTerminalResult(error, signal),
                meshName,
                targetNodeRid,
                packetName,
                correlationId,
                error
              );
            throw error;
          }
        );
        throwIfAborted(signal);
        if (node.isObjectClientNodeDirectTarget?.(toBackendRoutingId(targetNodeRid)) === true) {
          throw createInternalFrameworkException(
            ZLinkFrameworkInternalErrorKind.RequestTargetNotFound,
            `MeshNode '${meshName}' target '${targetNodeRid}' is an Object Client.`
          );
        }
        let parts: readonly MessageLike[] | undefined;
        try {
          parts = this.encodeMessage(
            ZLinkChannelMessageKind.Request,
            meshName,
            packetName,
            request,
            timeoutMs ?? DEFAULT_REQUEST_TIMEOUT_MS,
            metadata,
            correlationId
          );
        } catch (error) {
          requestOperation.fail(error);
          return;
        }
        try {
          this.completionTable(meshName).submit(
            (completion) => {
              const operationId = node.requestToNode(
                toBackendRoutingId(targetNodeRid),
                parts!,
                {
                  requestOperation,
                  flags: 1,
                  onAdmitted,
                  timeoutMs: requestTimeoutBudget(timeoutMs)
                },
                completion
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
            },
            signal,
            requestOperation,
            receiver
          );
        } finally {
          parts = undefined;
        }
      },
      (raw) => this.decodeMeshReply<TReply>(meshName, raw as ZLinkMeshCompletion),
      (error) =>
        mapMeshSubmissionError(error, `MeshNode '${meshName}' request to '${targetNodeRid}'`),
      existing
    );
    return result;
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
    requestOperation?: PendingOperation<unknown>
  ): ZLinkRequestResult<TReply> {
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
        requestOperation
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
        requestOperation
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
    existing?: PendingOperation<unknown>
  ): ZLinkRequestResult<TReply> {
    let metric: ReturnType<ZLinkRuntimeMetrics['startRequest']> | undefined;
    const result = this.requests.prepare<TReply>(
      timeoutMs ?? DEFAULT_REQUEST_TIMEOUT_MS,
      signal,
      (requestOperation, receiver) => {
        requestOperation.mapCaller(
          (value) => {
            metric?.complete('completed');
            return value;
          },
          (error) => {
            metric?.complete(requestMetricOutcome(error, signal));
            if (
              error instanceof ZLinkFrameworkException &&
              internalFrameworkErrorKind(error) ===
                ZLinkFrameworkInternalErrorKind.RequestTargetNotFound
            )
              this.metrics?.recordChannelSelectionFailure(meshName, channelName, 'no_ready_target');
            throw error;
          }
        );
        throwIfAborted(signal);
        const node = this.requireMeshNode(meshName);
        let parts: readonly MessageLike[] | undefined;
        try {
          parts = this.encodeMessage(
            ZLinkChannelMessageKind.Request,
            channelName,
            packetName,
            request,
            timeoutMs ?? DEFAULT_REQUEST_TIMEOUT_MS,
            metadata
          );
        } catch (error) {
          requestOperation.fail(error);
          return;
        }
        metric = this.metrics?.startRequest(meshName, 'channel');
        try {
          this.completionTable(meshName).submit(
            (completion) =>
              node.requestToChannel(
                channelName,
                parts!,
                {
                  requestOperation,
                  flags: 1,
                  timeoutMs: requestTimeoutBudget(timeoutMs),
                  onAdmitted
                },
                completion
              ),
            signal,
            requestOperation,
            receiver
          );
        } finally {
          parts = undefined;
        }
      },
      (raw) => this.decodeMeshReply<TReply>(meshName, raw as ZLinkMeshCompletion),
      (error) =>
        mapMeshSubmissionError(error, `MeshNode '${meshName}' channel '${channelName}' request`),
      existing
    );
    return result;
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
    options: ZLinkSpotRoutedRequestOptions,
    destination?: OperationObserver<unknown, unknown>
  ): ZLinkRequestResult<TReply> {
    return runWithOutboundFlow(this.flowCreationEnabled(), () =>
      this.requestToSpotScoped<TReply>(spotRouteTarget, request, options, destination)
    );
  }

  private requestToSpotScoped<TReply = unknown>(
    spotRouteTarget: ZLinkSpotRouteTarget,
    request: unknown,
    options: ZLinkSpotRoutedRequestOptions,
    destination?: OperationObserver<unknown, unknown>
  ): ZLinkRequestResult<TReply> {
    const meshName = spotRouteTarget.routerChannelId;
    const node = this.meshNode(meshName);
    if (node === undefined) {
      if (spotRouteTarget.spotKind === ZLinkSpotKind.Instance)
        throw new ZLinkConfigurationException(
          'Instance Spot routes require the MeshNode command 39 transport.'
        );
      return this.requests.prepare<TReply>(
        options.timeoutMs ?? DEFAULT_REQUEST_TIMEOUT_MS,
        options.signal,
        (operation, receiver) => {
          this.requireManager().routeRequestToSpot<TReply>(
            spotRouteTarget,
            options.packetName,
            request,
            options.timeoutMs,
            options.signal,
            options.metadata,
            options.onAdmitted,
            operation,
            receiver
          );
        },
        (value) => value as TReply,
        identityRequestReply,
        options.requestOperation,
        destination
      );
    }
    return this.requests.prepare<TReply>(
      options.timeoutMs ?? DEFAULT_REQUEST_TIMEOUT_MS,
      options.signal,
      (requestOperation, receiver) => {
        try {
          throwIfAborted(options.signal);
          const encoded = this.encodeMessage(
            ZLinkChannelMessageKind.Request,
            meshName,
            options.packetName,
            request,
            options.timeoutMs,
            options.metadata
          );
          this.completionTable(meshName).submit(
            (completion) =>
              spotRouteTarget.spotKind === ZLinkSpotKind.Instance
                ? node.requestInstanceSpot(
                    instanceSpotRouteFence(spotRouteTarget),
                    encoded,
                    options.timeoutMs,
                    undefined,
                    options.metadata,
                    options.instanceSpot,
                    options.onAdmitted,
                    requestOperation,
                    completion
                  )
                : node.entrySpot().requestToSpot(
                    toBackendRoutingId(spotRouteTarget.targetNodeRid),
                    toBackendRoutingId(spotRouteTarget.spotId),
                    spotRouteTarget.targetSpotGeneration ?? 0n,
                    encoded,
                    {
                      flags: 1,
                      onAdmitted: options.onAdmitted,
                      timeoutMs: options.timeoutMs,
                      requestOperation,
                      routeFence: directSpotRouteFence(spotRouteTarget),
                      entrySpot: spotRouteTarget.spotKind === ZLinkSpotKind.Entry
                    },
                    completion
                  ),
            options.signal,
            requestOperation,
            receiver
          );
        } catch (error) {
          receiver.operationFailed(requestOperation.id, error, undefined);
        }
      },
      (raw) => this.decodeMeshReply<TReply>(meshName, raw as ZLinkMeshCompletion),
      (error) =>
        mapMeshSubmissionError(
          error,
          `MeshNode '${meshName}' request to Spot '${spotRouteTarget.spotId}'`
        ),
      options.requestOperation,
      destination
    );
  }
  requestFromSpotToSpot<TReply = unknown>(
    sourceSpot: ZLinkBackendSpot,
    spotRouteTarget: ZLinkSpotRouteTarget,
    request: unknown,
    options: {
      readonly packetName?: string;
      readonly timeoutMs?: number;
      readonly requestOperation?: PendingOperation<unknown>;
      readonly signal?: AbortSignal;
      readonly onAdmitted?: () => void;
    },
    destination?: OperationObserver<unknown, unknown>
  ): ZLinkRequestResult<TReply> {
    return this.requests.prepare<TReply>(
      options.timeoutMs ?? DEFAULT_REQUEST_TIMEOUT_MS,
      options.signal,
      (operation, receiver) => {
        this.requireManager().routeRequestFromSpotToSpot<TReply>(
          sourceSpot,
          spotRouteTarget,
          options.packetName,
          request,
          options.timeoutMs,
          options.signal,
          options.onAdmitted,
          operation,
          receiver
        );
      },
      (value) => value as TReply,
      identityRequestReply,
      options.requestOperation,
      destination
    );
  }

  async requestRawFromSpotToSpot(
    sourceSpot: ZLinkBackendSpot,
    spotRouteTarget: ZLinkSpotRouteTarget,
    request: Message,
    options: {
      readonly timeoutMs?: number;
      readonly requestOperation?: PendingOperation<unknown>;
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
      options.onAdmitted,
      options.requestOperation
    );
  }

  requestRawToSpot(
    spotRouteTarget: ZLinkSpotRouteTarget,
    request: Message,
    options: {
      readonly timeoutMs?: number;
      readonly requestOperation?: PendingOperation<unknown>;
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
      readonly requestOperation?: PendingOperation<unknown>;
      readonly signal?: AbortSignal;
      readonly onAdmitted?: () => void;
    }
  ): Promise<readonly Message[]> {
    const meshName = spotRouteTarget.routerChannelId;
    const node = this.meshNode(meshName);
    if (node === undefined)
      return this.requireManager().routeRequestRawToSpot(
        spotRouteTarget,
        request,
        options.timeoutMs,
        options.signal,
        options.onAdmitted,
        options.requestOperation
      );
    const operation = this.requests.begin(
      options.timeoutMs ?? DEFAULT_REQUEST_TIMEOUT_MS,
      options.signal,
      options.requestOperation
    );
    return this.requests.run<readonly Message[]>(
      operation,
      (receiver) => {
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
        this.completionTable(meshName).submit(
          (completion) =>
            node.entrySpot().requestToSpot(
              toBackendRoutingId(spotRouteTarget.targetNodeRid),
              toBackendRoutingId(spotRouteTarget.spotId),
              spotRouteTarget.targetSpotGeneration ?? 0n,
              parts,
              {
                flags: 1,
                onAdmitted: options.onAdmitted,
                timeoutMs: options.timeoutMs,
                requestOperation: operation,
                routeFence: directSpotRouteFence(spotRouteTarget),
                entrySpot: spotRouteTarget.spotKind === ZLinkSpotKind.Entry
              },
              completion
            ),
          options.signal,
          operation,
          receiver
        );
      },
      (raw) => {
        const reply = this.decodeMeshReply<unknown>(meshName, raw as ZLinkMeshCompletion);
        const parts = [ZLinkBufferMessage.fromOwned(Buffer.from(JSON.stringify(reply)))];
        if (!operation.isPending()) closeMessages(parts);
        return parts;
      },
      (error) =>
        mapMeshSubmissionError(
          error,
          `MeshNode '${meshName}' raw request to Spot '${spotRouteTarget.spotId}'`
        )
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
    const runtime = this.meshRuntime?.();
    if (runtime === undefined) this.requireManager();
    return runtime?.meshNode?.(meshName);
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

function mapMeshSubmissionError(error: unknown, operation: string): unknown {
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
  if (
    isAbortError(error) ||
    error instanceof ZLinkConfigurationException ||
    error instanceof TypeError ||
    error instanceof RangeError
  ) {
    return error;
  }
  return new ZLinkFrameworkException(
    ZLinkFrameworkErrorKind.InternalFailure,
    `${operation} failed.`,
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
