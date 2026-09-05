import {
  ZLinkFrameworkInternalErrorKind,
  createInternalFrameworkException,
  internalFrameworkErrorKind,
  internalFrameworkErrorKindFromWireReply,
  isCanonicalWireReplyTerminal
} from '../framework-errors-internal';
import { ZLinkBufferMessage } from '../backend/runtime-message';
import {
  RequestResult,
  SubmitResult,
  isZLinkBackendResultError,
  type ZLinkBackendMessageLike as MessageLike
} from '../backend/runtime-values';
import type {
  ZLinkBackendMeshNode,
  ZLinkBackendSpot
} from '../backend/contracts';
import type {
  ZLinkFrameworkInternalErrorKind as ZLinkFrameworkInternalErrorKindType
} from '../framework-errors-internal';
import {
  ZLinkFrameworkException
} from '../../contracts';
import type { ZLinkFanoutListenerStatus } from '../../contracts';
import type { Message } from '../../contracts/Common/Message';
import {
  ZLinkSubmitStatus,
  type ZLinkSubmitResult
} from '../messaging/submission-result';
import { ZLinkConfigurationException } from '../configuration';
import type { ZLinkSpotRouteTarget } from '../spots/spot-routing-internal';
import { ZLinkSpotKind } from '../../contracts/Spots';
import { throwIfAborted } from '../abort';
import type { ZLinkRuntimeMetrics } from '../diagnostics';
import {
  closeMeshCompletion,
  type ZLinkMeshCompletion,
  type ZLinkMeshCompletionTable
} from '../backend/mesh-completion-table';
import { routingIdsEqual, toBackendRoutingId as toBackendRoutingId } from '../routing-id';
import {
  decodeChannelReply,
  encodeChannelEnvelopeParts,
  type ZLinkChannelEnvelopeCodecRegistry,
  ZLinkChannelMessageKind
} from './channel-envelope';
import type {
  ServiceDirectSpotRouteFence,
  ServiceInstanceRouteFence
} from '../foundation/service-stateful-wire-codec';
import { ServiceStaleGenerationError } from '../foundation/service-stateful-registry';
import { runWithOutboundFlow } from '../diagnostics/flow-context';

export interface ZLinkChannelClientTransport {
  send(
    channelName: string,
    packetName: string | undefined,
    message: unknown,
    signal?: AbortSignal,
    metadata?: ReadonlyMap<string, string>
  ): void | ZLinkSubmitResult | Promise<void | ZLinkSubmitResult>;
  request<TReply>(
    channelName: string,
    packetName: string | undefined,
    request: unknown,
    timeoutMs: number | undefined,
    signal?: AbortSignal,
    metadata?: ReadonlyMap<string, string>
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
  getFanoutListenerStatus?(channelName: string): ZLinkFanoutListenerStatus;
}

export type ZLinkChannelClientTransportSource =
  | ZLinkChannelClientTransport
  | (() => ZLinkChannelClientTransport | undefined);

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
    signal?: AbortSignal,
    metadata?: ReadonlyMap<string, string>
  ): void | ZLinkSubmitResult | Promise<void | ZLinkSubmitResult>;
  request<TReply>(
    routerChannelId: string,
    targetNodeRid: string,
    packetName: string | undefined,
    request: unknown,
    timeoutMs: number | undefined,
    signal?: AbortSignal,
    metadata?: ReadonlyMap<string, string>
  ): Promise<TReply>;
  submitToChannel(
    meshName: string,
    channelName: string,
    packetName: string | undefined,
    message: unknown,
    signal?: AbortSignal,
    metadata?: ReadonlyMap<string, string>
  ): void | ZLinkSubmitResult | Promise<void | ZLinkSubmitResult>;
  requestToChannel<TReply>(
    meshName: string,
    channelName: string,
    packetName: string | undefined,
    request: unknown,
    timeoutMs: number | undefined,
    signal?: AbortSignal,
    metadata?: ReadonlyMap<string, string>
  ): Promise<TReply>;
  sendToSpot?(
    spotRouteTarget: ZLinkSpotRouteTarget,
    message: unknown,
    options: {
      readonly packetName?: string;
      readonly timeoutMs?: number;
      readonly signal?: AbortSignal;
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
      readonly metadata?: ReadonlyMap<string, string>;
    }
  ): Promise<TReply>;
}

interface ZLinkChannelTransportRuntime {
  send(
    channelName: string,
    packetName: string | undefined,
    message: unknown,
    signal?: AbortSignal,
    metadata?: ReadonlyMap<string, string>
  ): Promise<ZLinkSubmitResult>;
  request<TReply>(
    channelName: string,
    packetName: string | undefined,
    request: unknown,
    timeoutMs: number | undefined,
    signal?: AbortSignal,
    metadata?: ReadonlyMap<string, string>
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
  getFanoutListenerStatus(channelName: string): ZLinkFanoutListenerStatus;
  canRouteChannel(routerChannelId: string): boolean;
  canRoutePacketChannel(routerChannelId: string): boolean;
  routeSubmit(
    routerChannelId: string,
    targetNodeRid: string,
    packetName: string | undefined,
    message: unknown,
    signal?: AbortSignal,
    metadata?: ReadonlyMap<string, string>
  ): Promise<ZLinkSubmitResult>;
  routeRequest<TReply>(
    routerChannelId: string,
    targetNodeRid: string,
    packetName: string | undefined,
    request: unknown,
    timeoutMs: number | undefined,
    signal?: AbortSignal,
    metadata?: ReadonlyMap<string, string>
  ): Promise<TReply>;
  routeSendToSpot(
    spotRouteTarget: ZLinkSpotRouteTarget,
    packetName: string | undefined,
    message: unknown,
    signal?: AbortSignal,
    metadata?: ReadonlyMap<string, string>,
    timeoutMs?: number
  ): Promise<void>;
  routeSendFromSpotToSpot(
    sourceSpot: ZLinkBackendSpot,
    spotRouteTarget: ZLinkSpotRouteTarget,
    packetName: string | undefined,
    message: unknown,
    signal?: AbortSignal,
    metadata?: ReadonlyMap<string, string>,
    timeoutMs?: number
  ): Promise<void>;
  routeRequestToSpot<TReply>(
    spotRouteTarget: ZLinkSpotRouteTarget,
    packetName: string | undefined,
    request: unknown,
    timeoutMs: number | undefined,
    signal?: AbortSignal,
    metadata?: ReadonlyMap<string, string>
  ): Promise<TReply>;
  routeRequestFromSpotToSpot<TReply>(
    sourceSpot: ZLinkBackendSpot,
    spotRouteTarget: ZLinkSpotRouteTarget,
    packetName: string | undefined,
    request: unknown,
    timeoutMs: number | undefined,
    signal?: AbortSignal
  ): Promise<TReply>;
  routeRequestRawFromSpotToSpot(
    sourceSpot: ZLinkBackendSpot,
    spotRouteTarget: ZLinkSpotRouteTarget,
    request: Message,
    timeoutMs: number | undefined,
    signal?: AbortSignal
  ): Promise<readonly Message[]>;
  routeRequestRawToSpot(
    spotRouteTarget: ZLinkSpotRouteTarget,
    request: Message,
    timeoutMs: number | undefined,
    signal?: AbortSignal
  ): Promise<readonly Message[]>;
}

export class ZLinkRuntimeRouteTransport implements ZLinkRouteClientTransport {
  constructor(
    private readonly manager: () => ZLinkChannelTransportRuntime | undefined,
    private readonly routeChannelPredicate: ((routerChannelId: string) => boolean) | undefined = undefined,
    private readonly meshRuntime: (() => {
      readonly meshNode: (meshName: string) => ZLinkBackendMeshNode | undefined;
      readonly meshCompletionTable: (meshName: string) => ZLinkMeshCompletionTable | undefined;
    } | undefined) | undefined = undefined,
    private readonly codecs?: ZLinkChannelEnvelopeCodecRegistry,
    private readonly manualNodeTarget?: (meshName: string, targetNodeRid: string) => boolean | undefined,
    private readonly localNodeSubmit?: (
      meshName: string,
      sourceNodeRid: string,
      parts: readonly MessageLike[]
    ) => ZLinkSubmitResult,
    private readonly metrics?: ZLinkRuntimeMetrics
  ) {}

  canRouteChannel(routerChannelId: string): boolean {
    const manager = this.manager();
    if (manager !== undefined) {
      return manager.canRouteChannel(routerChannelId);
    }
    return this.routeChannelPredicate?.(routerChannelId) ?? false;
  }

  canRoutePacketChannel(routerChannelId: string): boolean {
    return this.manager()?.canRoutePacketChannel(routerChannelId)
      ?? this.routeChannelPredicate?.(routerChannelId)
      ?? false;
  }

  async submit(
    meshName: string,
    targetNodeRid: string,
    packetName: string | undefined,
    message: unknown,
    signal?: AbortSignal,
    metadata?: ReadonlyMap<string, string>
  ): Promise<ZLinkSubmitResult> {
    // Call-scoped flow (spec 27 §4): the envelope flow pair lives only for
    // the duration of this outbound call, never in the caller's context.
    return runWithOutboundFlow(true, () => this.submitNode(
      meshName,
      targetNodeRid,
      packetName,
      message,
      signal,
      metadata,
      false
    ));
  }

  async submitInfrastructure(
    meshName: string,
    targetNodeRid: string,
    packetName: string | undefined,
    message: unknown,
    signal?: AbortSignal,
    metadata?: ReadonlyMap<string, string>
  ): Promise<ZLinkSubmitResult> {
    return runWithOutboundFlow(true, () => this.submitNode(
      meshName,
      targetNodeRid,
      packetName,
      message,
      signal,
      metadata,
      true
    ));
  }

  private async submitNode(
    meshName: string,
    targetNodeRid: string,
    packetName: string | undefined,
    message: unknown,
    signal: AbortSignal | undefined,
    metadata: ReadonlyMap<string, string> | undefined,
    allowObjectClientTarget: boolean
  ): Promise<ZLinkSubmitResult> {
    const node = this.meshNode(meshName);
    if (node === undefined) {
      return this.requireManager().routeSubmit(
        meshName,
        targetNodeRid,
        packetName,
        message,
        signal,
        metadata
      );
    }
    throwIfAborted(signal);
    if (!allowObjectClientTarget
      && node.isObjectClientNodeDirectTarget?.(toBackendRoutingId(targetNodeRid)) === true) {
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
    const operation = `MeshNode '${meshName}' send to node '${targetNodeRid}'`;
    if (this.isSelfNode(node, targetNodeRid)) {
      return this.submitLocalNode(meshName, String(node.status().routingId), parts);
    }
    if (!this.isKnownBackendPeer(node, targetNodeRid)
      && this.manualNodeTarget?.(meshName, targetNodeRid) === false) {
      return { status: ZLinkSubmitStatus.TargetNotFound };
    }
    try {
      return mapMeshSubmitResult(await node.sendToNode(
        toBackendRoutingId(targetNodeRid),
        parts,
        { flags: 1 }
      ));
    } catch (error) {
      throw mapMeshSubmissionError(error, operation);
    }
  }

  request<TReply>(
    meshName: string,
    targetNodeRid: string,
    packetName: string | undefined,
    request: unknown,
    timeoutMs: number | undefined,
    signal?: AbortSignal,
    metadata?: ReadonlyMap<string, string>
  ): Promise<TReply> {
    return runWithOutboundFlow(true, () => this.requestScoped<TReply>(
      meshName,
      targetNodeRid,
      packetName,
      request,
      timeoutMs,
      signal,
      metadata
    ));
  }

  private async requestScoped<TReply>(
    meshName: string,
    targetNodeRid: string,
    packetName: string | undefined,
    request: unknown,
    timeoutMs: number | undefined,
    signal?: AbortSignal,
    metadata?: ReadonlyMap<string, string>
  ): Promise<TReply> {
    const node = this.meshNode(meshName);
    if (node === undefined) {
      return this.requireManager().routeRequest(
        meshName,
        targetNodeRid,
        packetName,
        request,
        timeoutMs,
        signal,
        metadata
      );
    }
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
      timeoutMs ?? 30_000,
      metadata
    );
    const completion = await this.submitRequestOperation(
      meshName,
      timeoutMs,
      signal,
      `MeshNode '${meshName}' request to node '${targetNodeRid}'`,
      (remainingTimeoutMs) => node.requestToNode(
        toBackendRoutingId(targetNodeRid),
        parts,
        { flags: 1, timeoutMs: remainingTimeoutMs }
      )
    );
    return this.decodeMeshReply(meshName, completion);
  }

  submitToChannel(
    meshName: string,
    channelName: string,
    packetName: string | undefined,
    message: unknown,
    signal?: AbortSignal,
    metadata?: ReadonlyMap<string, string>
  ): Promise<ZLinkSubmitResult> {
    return runWithOutboundFlow(true, () => this.submitToChannelScoped(
      meshName,
      channelName,
      packetName,
      message,
      signal,
      metadata
    ));
  }

  private async submitToChannelScoped(
    meshName: string,
    channelName: string,
    packetName: string | undefined,
    message: unknown,
    signal?: AbortSignal,
    metadata?: ReadonlyMap<string, string>
  ): Promise<ZLinkSubmitResult> {
    throwIfAborted(signal);
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
      const result = mapMeshSubmitResult(await node.sendToChannel(
        channelName,
        parts,
        { flags: 1 }
      ));
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
    metadata?: ReadonlyMap<string, string>
  ): Promise<TReply> {
    return runWithOutboundFlow(true, () => this.requestToChannelScoped<TReply>(
      meshName,
      channelName,
      packetName,
      request,
      timeoutMs,
      signal,
      metadata
    ));
  }

  private async requestToChannelScoped<TReply>(
    meshName: string,
    channelName: string,
    packetName: string | undefined,
    request: unknown,
    timeoutMs: number | undefined,
    signal?: AbortSignal,
    metadata?: ReadonlyMap<string, string>
  ): Promise<TReply> {
    throwIfAborted(signal);
    const node = this.requireMeshNode(meshName);
    const parts = this.encodeMessage(
      ZLinkChannelMessageKind.Request,
      channelName,
      packetName,
      request,
      timeoutMs ?? 30_000,
      metadata
    );
    const metric = this.metrics?.startRequest(meshName, 'channel');
    try {
      const completion = await this.submitRequestOperation(
        meshName,
        timeoutMs,
        signal,
        `MeshNode '${meshName}' request to channel '${channelName}'`,
        (remainingTimeoutMs) => node.requestToChannel(
          channelName,
          parts,
          { flags: 1, timeoutMs: remainingTimeoutMs }
        )
      );
      const reply = this.decodeMeshReply<TReply>(meshName, completion);
      metric?.complete('completed');
      return reply;
    } catch (error) {
      metric?.complete(requestMetricOutcome(error));
      if (requestMetricOutcome(error) === 'target_not_found') {
        this.metrics?.recordChannelSelectionFailure(meshName, channelName, 'no_ready_target');
      }
      throw error;
    }
  }

  private async submitRequestOperation(
    meshName: string,
    timeoutMs: number | undefined,
    signal: AbortSignal | undefined,
    operation: string,
    attempt: (
      remainingTimeoutMs: number
    ) => ReturnType<Parameters<ZLinkMeshCompletionTable['submit']>[0]>
  ): Promise<ZLinkMeshCompletion> {
    const effectiveTimeoutMs = Math.max(1, timeoutMs ?? 30_000);
    const deadlineMs = performance.now() + effectiveTimeoutMs;
    const remainingTimeoutMs = deadlineMs - performance.now();
    if (remainingTimeoutMs <= 0) {
      throw createInternalFrameworkException(
        ZLinkFrameworkInternalErrorKind.DeadlineExceeded,
        `${operation} timed out before submission.`
      );
    }
    try {
      return await this.completionTable(meshName).submit(
        () => attempt(Math.max(1, Math.ceil(remainingTimeoutMs))),
        signal
      );
    } catch (error) {
      throw mapMeshSubmissionError(error, operation);
    }
  }

  sendToSpot(
    spotRouteTarget: ZLinkSpotRouteTarget,
    message: unknown,
    options: {
      readonly packetName?: string;
      readonly timeoutMs?: number;
      readonly signal?: AbortSignal;
      readonly metadata?: ReadonlyMap<string, string>;
    }
  ): Promise<ZLinkSubmitResult> {
    return runWithOutboundFlow(true, () => this.sendToSpotScoped(spotRouteTarget, message, options));
  }

  private async sendToSpotScoped(
    spotRouteTarget: ZLinkSpotRouteTarget,
    message: unknown,
    options: {
      readonly packetName?: string;
      readonly timeoutMs?: number;
      readonly signal?: AbortSignal;
      readonly metadata?: ReadonlyMap<string, string>;
    }
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
        options.signal,
        options.metadata,
        options.timeoutMs
      );
      return { status: ZLinkSubmitStatus.Submitted };
    }
    throwIfAborted(options.signal);
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
        return mapMeshSubmitResult(await node.sendToInstanceSpot(
          instanceSpotRouteFence(spotRouteTarget),
          encoded,
          undefined,
          options.metadata
        ));
      }
      return mapMeshSubmitResult(await node.entrySpot().sendToSpot(
          toBackendRoutingId(spotRouteTarget.targetNodeRid),
          toBackendRoutingId(spotRouteTarget.spotId),
          spotRouteTarget.targetSpotGeneration ?? 0n,
          encoded,
          {
            flags: 1,
            routeFence: directSpotRouteFence(spotRouteTarget),
            entrySpot: spotRouteTarget.spotKind === ZLinkSpotKind.Entry
          }
      ));
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
      readonly timeoutMs?: number;
      readonly signal?: AbortSignal;
      readonly metadata?: ReadonlyMap<string, string>;
    }
  ): Promise<ZLinkSubmitResult> {
    await this.requireManager().routeSendFromSpotToSpot(
      sourceSpot,
      spotRouteTarget,
      options.packetName,
      message,
      options.signal,
      options.metadata,
      options.timeoutMs
    );
    return { status: ZLinkSubmitStatus.Submitted };
  }

  requestToSpot<TReply = unknown>(
    spotRouteTarget: ZLinkSpotRouteTarget,
    request: unknown,
    options: {
      readonly packetName?: string;
      readonly timeoutMs?: number;
      readonly signal?: AbortSignal;
      readonly metadata?: ReadonlyMap<string, string>;
    }
  ): Promise<TReply> {
    return runWithOutboundFlow(true, () => this.requestToSpotScoped<TReply>(spotRouteTarget, request, options));
  }

  private async requestToSpotScoped<TReply = unknown>(
    spotRouteTarget: ZLinkSpotRouteTarget,
    request: unknown,
    options: {
      readonly packetName?: string;
      readonly timeoutMs?: number;
      readonly signal?: AbortSignal;
      readonly metadata?: ReadonlyMap<string, string>;
    }
  ): Promise<TReply> {
    const meshName = spotRouteTarget.routerChannelId;
    const node = this.meshNode(meshName);
    if (node === undefined) {
      if (spotRouteTarget.spotKind === ZLinkSpotKind.Instance) {
        throw new ZLinkConfigurationException(
          'Instance Spot routes require the MeshNode command 39 transport.'
        );
      }
      return this.requireManager().routeRequestToSpot<TReply>(
        spotRouteTarget,
        options.packetName,
        request,
        options.timeoutMs,
        options.signal,
        options.metadata
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
      completionPromise = this.completionTable(meshName).submit(() =>
        spotRouteTarget.spotKind === ZLinkSpotKind.Instance
          ? node.requestInstanceSpot(
              instanceSpotRouteFence(spotRouteTarget),
              encoded,
              options.timeoutMs,
              undefined,
              options.metadata
            )
          : node.entrySpot().requestToSpot(
              toBackendRoutingId(spotRouteTarget.targetNodeRid),
              toBackendRoutingId(spotRouteTarget.spotId),
              spotRouteTarget.targetSpotGeneration ?? 0n,
              encoded,
              {
                flags: 1,
                timeoutMs: options.timeoutMs,
                routeFence: directSpotRouteFence(spotRouteTarget),
                entrySpot: spotRouteTarget.spotKind === ZLinkSpotKind.Entry
              }
            ), options.signal);
    } catch (error) {
      throw mapMeshSubmissionError(
        error,
        `MeshNode '${meshName}' request to Spot '${spotRouteTarget.spotId}'`
      );
    }
    return this.waitForMeshReply<TReply>(meshName, completionPromise);
  }

  async requestFromSpotToSpot<TReply = unknown>(
    sourceSpot: ZLinkBackendSpot,
    spotRouteTarget: ZLinkSpotRouteTarget,
    request: unknown,
    options: { readonly packetName?: string; readonly timeoutMs?: number; readonly signal?: AbortSignal }
  ): Promise<TReply> {
    return this.requireManager().routeRequestFromSpotToSpot<TReply>(
      sourceSpot,
      spotRouteTarget,
      options.packetName,
      request,
      options.timeoutMs,
      options.signal
    );
  }

  async requestRawFromSpotToSpot(
    sourceSpot: ZLinkBackendSpot,
    spotRouteTarget: ZLinkSpotRouteTarget,
    request: Message,
    options: { readonly timeoutMs?: number; readonly signal?: AbortSignal }
  ): Promise<readonly Message[]> {
    return this.requireManager().routeRequestRawFromSpotToSpot(
      sourceSpot,
      spotRouteTarget,
      request,
      options.timeoutMs,
      options.signal
    );
  }

  requestRawToSpot(
    spotRouteTarget: ZLinkSpotRouteTarget,
    request: Message,
    options: { readonly timeoutMs?: number; readonly signal?: AbortSignal }
  ): Promise<readonly Message[]> {
    return runWithOutboundFlow(true, () => this.requestRawToSpotScoped(spotRouteTarget, request, options));
  }

  private async requestRawToSpotScoped(
    spotRouteTarget: ZLinkSpotRouteTarget,
    request: Message,
    options: { readonly timeoutMs?: number; readonly signal?: AbortSignal }
  ): Promise<readonly Message[]> {
    const meshName = spotRouteTarget.routerChannelId;
    const node = this.meshNode(meshName);
    if (node !== undefined) {
      throwIfAborted(options.signal);
      const payload = JSON.parse(request.getString('utf8')) as Record<string, unknown>;
      const packetName = typeof payload.packetName === 'string'
        ? payload.packetName
        : undefined;
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
          () => node.entrySpot().requestToSpot(
            toBackendRoutingId(spotRouteTarget.targetNodeRid),
            toBackendRoutingId(spotRouteTarget.spotId),
            spotRouteTarget.targetSpotGeneration ?? 0n,
            parts,
            {
              flags: 1,
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
      options.signal
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
    return (
      runtime as unknown as {
        meshNode?: (meshName: string) => ZLinkBackendMeshNode | undefined;
      } | undefined
    )?.meshNode?.(meshName);
  }

  private isSelfNode(node: ZLinkBackendMeshNode, targetNodeRid: string): boolean {
    return routingIdsEqual(
      node.status().routingId as unknown as import('../../contracts').RoutingId,
      targetNodeRid
    );
  }

  private isKnownBackendPeer(node: ZLinkBackendMeshNode, targetNodeRid: string): boolean {
    return node.peers().some((peer) => peer.routingId !== null && routingIdsEqual(
      peer.routingId as unknown as import('../../contracts').RoutingId,
      targetNodeRid
    ));
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
      throw new ZLinkConfigurationException(`MeshNode '${meshName}' completion runtime is not started.`);
    }
    return table;
  }

  private encodeMessage(
    kind: ZLinkChannelMessageKind,
    channelName: string,
    packetName: string | undefined,
    message: unknown,
    timeoutMs?: number,
    metadata?: ReadonlyMap<string, string>
  ): readonly MessageLike[] {
    return encodeChannelEnvelopeParts(
      kind,
      channelName,
      packetName,
      message,
      timeoutMs,
      undefined,
      this.codecs,
      undefined,
      true,
      metadata
    );
  }

  private async waitForMeshReply<TReply>(
    meshName: string,
    completionPromise: Promise<ZLinkMeshCompletion>
  ): Promise<TReply> {
    const completion = await completionPromise;
    return this.decodeMeshReply<TReply>(meshName, completion);
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

function directSpotRouteFence(target: ZLinkSpotRouteTarget): ServiceDirectSpotRouteFence | undefined {
  // Entry Spots are addressed by their owning MeshNode. They have no
  // Location Store authority row, so only User and Instance Spots carry the
  // exact Ready authority fence required by direct Spot submission.
  if (target.spotKind === ZLinkSpotKind.Entry) {
    return undefined;
  }
  if (
    target.targetSpotGeneration === undefined
    || target.targetNodeGeneration === undefined
    || target.authorityOwnerGeneration === undefined
    || target.targetOwnerId === undefined
    || target.ownerLeaseGeneration === undefined
    || target.authorityStoreVersion === undefined
  ) {
    throw createInternalFrameworkException(
      ZLinkFrameworkInternalErrorKind.SpotRouteNotFound,
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
    target.targetSpotGeneration === undefined
    || target.targetNodeGeneration === undefined
    || target.authorityOwnerGeneration === undefined
    || target.targetOwnerId === undefined
    || target.ownerLeaseGeneration === undefined
    || target.authorityStoreVersion === undefined
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

function mapMeshSubmitResult(result: number): ZLinkSubmitResult {
  switch (result) {
    case SubmitResult.Ok:
      return { status: ZLinkSubmitStatus.Submitted };
    case SubmitResult.Backpressured:
    case SubmitResult.NotAdmitted:
      return { status: ZLinkSubmitStatus.Backpressured };
    case SubmitResult.NotFound:
      return { status: ZLinkSubmitStatus.TargetNotFound };
    case SubmitResult.NotConnected:
      return { status: ZLinkSubmitStatus.RouteNotConnected };
    case SubmitResult.Terminated:
      return { status: ZLinkSubmitStatus.Shutdown };
    default:
      throw createInternalFrameworkException(
        ZLinkFrameworkInternalErrorKind.RequestFailed,
        `Mesh submission failed with result ${result}.`
      );
  }
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
    const notFound = error.result === SubmitResult.NotFound || error.result === 102;
    const retriable = error.result === SubmitResult.Backpressured
      || error.result === SubmitResult.NotConnected
      || error.result === 109
      || error.result === 113;
    return createInternalFrameworkException(
      notFound
        ? ZLinkFrameworkInternalErrorKind.RequestTargetNotFound
        : ZLinkFrameworkInternalErrorKind.RouteNotConnected,
      `${operation} failed with result ${error.result}.`,
      retriable,
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

function meshRequestFailure(meshName: string, result: number, nativeErrno: number): ZLinkFrameworkException {
  const canonical = isCanonicalWireReplyTerminal(result, nativeErrno);
  const wireKind = canonical
    ? internalFrameworkErrorKindFromWireReply(result, nativeErrno)
    : undefined;
  const kind: ZLinkFrameworkInternalErrorKindType = !canonical
    ? ZLinkFrameworkInternalErrorKind.RequestProtocolError
    : result === RequestResult.NotFound
      ? ZLinkFrameworkInternalErrorKind.RequestTargetNotFound
      : result === RequestResult.TimedOut
        ? ZLinkFrameworkInternalErrorKind.DeadlineExceeded
        : result === RequestResult.Terminated
          ? ZLinkFrameworkInternalErrorKind.RuntimeShutdown
          : result === RequestResult.Conflict || result === RequestResult.InternalError
            ? wireKind ?? ZLinkFrameworkInternalErrorKind.RequestProtocolError
            : result === RequestResult.NotConnected || result === RequestResult.Backpressured
              ? ZLinkFrameworkInternalErrorKind.RouteNotConnected
              : wireKind ?? ZLinkFrameworkInternalErrorKind.RequestFailed;
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

function requestMetricOutcome(error: unknown): string {
  if (error instanceof ZLinkFrameworkException) {
    if (internalFrameworkErrorKind(error) === ZLinkFrameworkInternalErrorKind.RequestTargetNotFound) return 'target_not_found';
    if (internalFrameworkErrorKind(error) === ZLinkFrameworkInternalErrorKind.DeadlineExceeded) return 'timed_out';
    if (internalFrameworkErrorKind(error) === ZLinkFrameworkInternalErrorKind.RuntimeShutdown) return 'shutdown';
  }
  return 'failed';
}
