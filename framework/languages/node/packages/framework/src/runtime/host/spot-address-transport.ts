import { ZLinkFrameworkException, ZLinkSpotKind, type RoutingId } from '../../contracts';
import {
  ZLinkRuntimeDispatchErrorAction as ZLinkDispatchErrorAction,
  ZLinkRuntimeDispatchErrorReason as ZLinkDispatchErrorReason,
  ZLinkDispatchErrorSurface,
  ZLinkDispatchMessageKind,
  ZLinkRuntimeMessageFlowOutcome as ZLinkMessageFlowOutcome
} from '../../contracts/Dispatch/ZLinkDispatchOptions';
import { awaitWithAbort } from '../abort';
import type { ZLinkBackendMeshNode } from '../backend/contracts';
import {
  closeMeshCompletion,
  type ZLinkMeshCompletionTable
} from '../backend/mesh-completion-table';
import {
  RequestResult,
  type ZLinkBackendMessageLike as MessageLike
} from '../backend/runtime-values';
import type { ZLinkDispatchErrorReporter } from '../channels';
import {
  ZLinkChannelMessageKind,
  decodeChannelReply,
  encodeChannelEnvelopeParts,
  encodeChannelEnvelopePartsAtDeadline,
  type ZLinkChannelEnvelopeCodecRegistry
} from '../channels/channel-envelope';
import { flowIfEnabled } from '../diagnostics';
import { runWithOutboundFlow } from '../diagnostics/flow-context';
import {
  ZLinkFrameworkInternalErrorKind,
  createInternalFrameworkException,
  internalFrameworkErrorKind,
  internalFrameworkErrorKindFromWireReply,
  isCanonicalWireReplyTerminal
} from '../framework-errors-internal';
import { resolveFrameworkPacketName } from '../messaging/packet-name';
import {
  ZLinkSubmitStatus,
  classifySubmitResult,
  type ZLinkSubmitResult
} from '../messaging/submission-result';
import type {
  ZLinkSpotAddressCallOptions,
  ZLinkSpotAddressTransport,
  ZLinkSpotRoutedTransport
} from '../spots/spot-outbound';
import type { ZLinkSpotRouteResolver, ZLinkSpotRouteTarget } from '../spots/spot-routing-internal';

export interface ZLinkHostSpotAddressTransportOptions {
  readonly resolver: () => ZLinkSpotRouteResolver | undefined;
  readonly routed: ZLinkSpotRoutedTransport;
  readonly meshNames: () => readonly string[];
  readonly isMeshConfigured?: (meshName: string) => boolean;
  readonly meshNode: (meshName: string) => ZLinkBackendMeshNode | undefined;
  readonly completions: (meshName: string) => ZLinkMeshCompletionTable | undefined;
  readonly codecs?: ZLinkChannelEnvelopeCodecRegistry;
  readonly defaultRequestTimeoutMs: number;
  readonly dispatchErrors?: ZLinkDispatchErrorReporter;
}

export function hasObjectClientCapability(role: 'none' | 'client' | 'server' | undefined): boolean {
  return role === 'client' || role === 'server';
}

type MissingTarget = {
  readonly meshName: string;
  readonly node: ZLinkBackendMeshNode;
  readonly target: {
    readonly targetNodeRid: string;
    readonly targetNodeGeneration: bigint;
    readonly targetSpotId: string;
    readonly stableType: string;
    readonly descriptorVersion: string;
  };
};

type MissingTargetSelection =
  | ({ readonly kind: 'selected' } & MissingTarget)
  | { readonly kind: 'unsupported' }
  | { readonly kind: 'capacity' }
  | { readonly kind: 'unavailable' };

/** Owns global Spot authority lookup and Missing Instance placement. */
export class ZLinkHostSpotAddressTransport implements ZLinkSpotAddressTransport {
  constructor(private readonly options: ZLinkHostSpotAddressTransportOptions) {}

  sendToSpotAddress(
    spotId: RoutingId,
    message: unknown,
    call: Omit<ZLinkSpotAddressCallOptions, 'timeoutMs' | 'signal'>
  ): Promise<ZLinkSubmitResult> {
    // Call-scoped flow (spec 27 §4): the envelope encoders and the
    // traceInstanceAddress points share one ambient flow that does not
    // outlive this call.
    return runWithOutboundFlow(
      this.options.dispatchErrors?.flow.flowCreationEnabled() ?? true,
      () => this.sendToSpotAddressScoped(spotId, message, call)
    );
  }

  private async sendToSpotAddressScoped(
    spotId: RoutingId,
    message: unknown,
    call: Omit<ZLinkSpotAddressCallOptions, 'timeoutMs' | 'signal'>
  ): Promise<ZLinkSubmitResult> {
    const activationDeadlineUnixMs = BigInt(
      Math.round(Date.now() + this.options.defaultRequestTimeoutMs)
    );
    const existing = await this.resolveExisting(spotId);
    if (existing !== undefined) {
      this.validateExisting(existing, call);
      try {
        const result = await this.options.routed.sendToSpot(existing, message, {
          instanceSpot: call.instanceSpot,
          metadata: call.metadata
        });
        if (result.status === ZLinkSubmitStatus.Submitted) {
          this.traceInstanceAddress(
            ZLinkMessageFlowOutcome.Sent,
            ZLinkDispatchMessageKind.Send,
            spotId,
            message,
            existing.routerChannelId,
            existing.stableType,
            existing.targetNodeRid
          );
        } else {
          this.traceInstanceAddress(
            submitResultFlowOutcome(result.status),
            ZLinkDispatchMessageKind.Send,
            spotId,
            message,
            existing.routerChannelId,
            existing.stableType,
            existing.targetNodeRid,
            submitResultReason(result.status)
          );
        }
        if (
          result.status === ZLinkSubmitStatus.TargetNotFound ||
          result.status === ZLinkSubmitStatus.RouteNotConnected
        ) {
          this.options.resolver()?.invalidate?.(spotId);
        }
        return result;
      } catch (error) {
        if (isSpotRouteRefreshError(error)) {
          this.options.resolver()?.invalidate?.(spotId);
        }
        const reason = addressedInstanceErrorReason(error);
        this.traceInstanceAddress(
          reason === ZLinkDispatchErrorReason.Backpressure
            ? ZLinkMessageFlowOutcome.Backpressured
            : ZLinkMessageFlowOutcome.Dropped,
          ZLinkDispatchMessageKind.Send,
          spotId,
          message,
          existing.routerChannelId,
          existing.stableType,
          existing.targetNodeRid,
          reason
        );
        throw error;
      }
    }
    if (!call.instanceSpot) {
      return { status: ZLinkSubmitStatus.TargetNotFound };
    }
    const selected = this.selectMissingTarget(spotId, call);
    if (selected.kind === 'unsupported') {
      this.traceInstanceAddress(
        ZLinkMessageFlowOutcome.Dropped,
        ZLinkDispatchMessageKind.Send,
        spotId,
        message,
        call.initialMeshName,
        call.instanceSpotType,
        undefined,
        ZLinkDispatchErrorReason.StaleTarget
      );
      return { status: ZLinkSubmitStatus.TargetNotFound };
    }
    if (selected.kind === 'capacity') {
      const error = missingInstancePlacementCapacity(spotId, call.instanceSpotType);
      this.traceInstanceAddress(
        ZLinkMessageFlowOutcome.Backpressured,
        ZLinkDispatchMessageKind.Send,
        spotId,
        message,
        call.initialMeshName,
        call.instanceSpotType,
        undefined,
        ZLinkDispatchErrorReason.Backpressure
      );
      throw error;
    }
    if (selected.kind === 'unavailable') {
      this.traceInstanceAddress(
        ZLinkMessageFlowOutcome.Dropped,
        ZLinkDispatchMessageKind.Send,
        spotId,
        message,
        call.initialMeshName,
        call.instanceSpotType,
        undefined,
        ZLinkDispatchErrorReason.StaleTarget
      );
      return { status: ZLinkSubmitStatus.RouteNotConnected };
    }
    const encoded = this.encode(ZLinkChannelMessageKind.Command, selected.meshName, message);
    const sourceSpotId =
      call.sourceSpot === undefined ? undefined : String(call.sourceSpot.routingId);
    const mapped = classifySubmitResult(
      await selected.node.sendToMissingInstanceSpot(
        selected.target,
        encoded,
        activationDeadlineUnixMs,
        sourceSpotId,
        call.metadata
      ),
      'Instance Spot submission'
    );
    this.traceInstanceAddress(
      submitResultFlowOutcome(mapped.status),
      ZLinkDispatchMessageKind.Send,
      spotId,
      message,
      selected.meshName,
      selected.target.stableType,
      selected.target.targetNodeRid,
      mapped.status === ZLinkSubmitStatus.Submitted ? undefined : submitResultReason(mapped.status)
    );
    return mapped;
  }

  requestToSpotAddress<TReply = unknown>(
    spotId: RoutingId,
    request: unknown,
    call: ZLinkSpotAddressCallOptions
  ): Promise<TReply> {
    return runWithOutboundFlow(
      this.options.dispatchErrors?.flow.flowCreationEnabled() ?? true,
      () => this.requestToSpotAddressScoped<TReply>(spotId, request, call)
    );
  }

  private async requestToSpotAddressScoped<TReply = unknown>(
    spotId: RoutingId,
    request: unknown,
    call: ZLinkSpotAddressCallOptions
  ): Promise<TReply> {
    const timeoutMs = call.timeoutMs ?? this.options.defaultRequestTimeoutMs;
    const deadline = createSpotAddressDeadline(timeoutMs, call.signal);
    try {
      const existing = await this.resolveExisting(spotId, deadline.signal);
      deadline.requireRemaining();
      if (existing === undefined) {
        return await this.requestToMissingInstance(spotId, request, call, deadline);
      }
      this.validateExisting(existing, call);
      try {
        return await this.requestToExistingSpot(spotId, request, call, existing, deadline);
      } catch (error) {
        if (isSpotRouteRefreshError(error)) {
          this.options.resolver()?.invalidate?.(spotId);
        }
        // Failover policy §4.4: SpotMoving is the owner's Ready owner-fence
        // refusal, made before admission. With Instance intent, one authority
        // read under the same deadline decides: Missing means Close released
        // the incarnation, so the request activates a new one. Any other
        // answer, and any message the owner admitted, keeps this terminal.
        if (
          call.instanceSpot &&
          isOwnerFenceRefusal(error) &&
          (await this.resolveExisting(spotId, deadline.signal)) === undefined
        ) {
          deadline.requireRemaining();
          return await this.requestToMissingInstance(spotId, request, call, deadline);
        }
        this.reportInstanceRequestError(
          spotId,
          request,
          error,
          existing.routerChannelId,
          existing.targetNodeRid,
          existing.stableType,
          addressedInstanceErrorReason(error)
        );
        throw error;
      }
    } catch (error) {
      if (
        deadline.expired() &&
        !(
          error instanceof ZLinkFrameworkException &&
          internalFrameworkErrorKind(error) ===
            ZLinkFrameworkInternalErrorKind.RequestTargetNotFound
        )
      ) {
        throw createInternalFrameworkException(
          ZLinkFrameworkInternalErrorKind.DeadlineExceeded,
          `Spot request for '${String(spotId)}' exceeded its end-to-end deadline.`,
          true,
          error
        );
      }
      throw error;
    } finally {
      deadline.close();
    }
  }

  private async requestToExistingSpot<TReply>(
    spotId: RoutingId,
    request: unknown,
    call: ZLinkSpotAddressCallOptions,
    existing: ZLinkSpotRouteTarget,
    deadline: ZLinkSpotAddressDeadline
  ): Promise<TReply> {
    this.traceInstanceAddress(
      ZLinkMessageFlowOutcome.Sent,
      ZLinkDispatchMessageKind.Request,
      spotId,
      request,
      existing.routerChannelId,
      existing.stableType,
      existing.targetNodeRid
    );
    const reply = await awaitWithAbort(
      this.options.routed.requestToSpot<TReply>(existing, request, {
        instanceSpot: call.instanceSpot,
        timeoutMs: deadline.requireRemaining(),
        signal: deadline.signal,
        metadata: call.metadata
      }),
      deadline.signal
    );
    this.traceInstanceAddress(
      ZLinkMessageFlowOutcome.ReplyReceived,
      ZLinkDispatchMessageKind.Request,
      spotId,
      request,
      existing.routerChannelId,
      existing.stableType,
      existing.targetNodeRid
    );
    return reply;
  }

  private async requestToMissingInstance<TReply>(
    spotId: RoutingId,
    request: unknown,
    call: ZLinkSpotAddressCallOptions,
    deadline: ZLinkSpotAddressDeadline
  ): Promise<TReply> {
    if (!call.instanceSpot) {
      throw createInternalFrameworkException(
        ZLinkFrameworkInternalErrorKind.RequestTargetNotFound,
        `Spot '${String(spotId)}' has no Ready authority.`
      );
    }
    const selected = this.selectMissingTarget(spotId, call);
    if (selected.kind === 'capacity') {
      throw missingInstancePlacementCapacity(spotId, call.instanceSpotType);
    }
    if (selected.kind === 'unsupported') {
      const error = createInternalFrameworkException(
        ZLinkFrameworkInternalErrorKind.RequestTargetNotFound,
        `No eligible Instance Spot target serves '${String(spotId)}'.`
      );
      this.reportInstanceRequestError(
        spotId,
        request,
        error,
        call.initialMeshName,
        undefined,
        call.instanceSpotType,
        ZLinkDispatchErrorReason.StaleTarget
      );
      throw error;
    }
    if (selected.kind === 'unavailable') {
      const error = missingInstancePlacementUnavailable(spotId, call.instanceSpotType);
      this.reportInstanceRequestError(
        spotId,
        request,
        error,
        call.initialMeshName,
        undefined,
        call.instanceSpotType,
        ZLinkDispatchErrorReason.StaleTarget
      );
      throw error;
    }
    const deadlineUnixMs = BigInt(deadline.deadlineUnixMs);
    const encoded = this.encodeAtDeadline(
      ZLinkChannelMessageKind.Request,
      selected.meshName,
      request,
      deadline.deadlineUnixMs
    );
    deadline.requireRemaining();
    const table = this.options.completions(selected.meshName);
    if (table === undefined)
      throw new Error(`MeshNode '${selected.meshName}' completion table is not started.`);
    const completionPromise = table.submit(
      () =>
        selected.node.requestToMissingInstanceSpot(
          selected.target,
          encoded,
          deadlineUnixMs,
          call.sourceSpot === undefined ? undefined : String(call.sourceSpot.routingId),
          call.metadata
        ),
      deadline.signal
    );
    this.traceInstanceAddress(
      ZLinkMessageFlowOutcome.Sent,
      ZLinkDispatchMessageKind.Request,
      spotId,
      request,
      selected.meshName,
      selected.target.stableType,
      selected.target.targetNodeRid
    );
    const completion = await awaitWithAbort(completionPromise, deadline.signal);
    try {
      if (completion.terminalResult !== 0 || completion.failureErrno !== 0) {
        const error = missingInstanceRequestFailure(
          completion.terminalResult,
          completion.failureErrno
        );
        this.reportInstanceRequestError(
          spotId,
          request,
          error,
          selected.meshName,
          selected.target.targetNodeRid,
          selected.target.stableType,
          addressedInstanceErrorReason(error)
        );
        throw error;
      }
      const reply = decodeChannelReply<TReply>(completion.parts, this.options.codecs);
      this.traceInstanceAddress(
        ZLinkMessageFlowOutcome.ReplyReceived,
        ZLinkDispatchMessageKind.Request,
        spotId,
        request,
        selected.meshName,
        selected.target.stableType,
        selected.target.targetNodeRid
      );
      return reply;
    } finally {
      closeMeshCompletion(completion);
    }
  }

  private reportInstanceRequestError(
    spotId: RoutingId,
    request: unknown,
    error: unknown,
    meshName: string | undefined,
    targetRid: string | undefined,
    instanceSpotType: string | undefined,
    reason: ZLinkDispatchErrorReason
  ): void {
    this.options.dispatchErrors?.report({
      surface: ZLinkDispatchErrorSurface.InstanceSpot,
      messageKind: ZLinkDispatchMessageKind.Request,
      packetName: resolveFrameworkPacketName(request, undefined, 'Channel'),
      meshName,
      targetRid,
      spotId: String(spotId),
      instanceSpotType,
      reason,
      action: ZLinkDispatchErrorAction.FailCaller,
      error
    });
  }

  private traceInstanceAddress(
    outcome: ZLinkMessageFlowOutcome,
    messageKind: ZLinkDispatchMessageKind,
    spotId: RoutingId,
    message: unknown,
    meshName: string | undefined,
    instanceSpotType: string | undefined,
    targetRid: string | undefined,
    errorReason?: ZLinkDispatchErrorReason
  ): void {
    const flow = flowIfEnabled(this.options.dispatchErrors?.flow, outcome);
    if (flow === undefined) return;
    flow.trace({
      outcome,
      surface: ZLinkDispatchErrorSurface.InstanceSpot,
      messageKind,
      packetName: resolveFrameworkPacketName(message, undefined, 'Channel'),
      meshName,
      targetRid,
      spotId: String(spotId),
      instanceSpotType,
      errorReason
    });
  }

  private async resolveExisting(
    spotId: RoutingId,
    signal?: AbortSignal
  ): Promise<import('../spots/spot-routing-internal').ZLinkSpotRouteTarget | undefined> {
    const resolver = this.options.resolver();
    if (resolver === undefined) {
      throw new Error('Global Spot address resolution requires a Location Store.');
    }
    try {
      return await awaitWithAbort(resolver.resolve(spotId, signal), signal);
    } catch (error) {
      if (
        error instanceof ZLinkFrameworkException &&
        internalFrameworkErrorKind(error) === ZLinkFrameworkInternalErrorKind.SpotRouteNotFound
      ) {
        return undefined;
      }
      throw error;
    }
  }

  private selectMissingTarget(
    spotId: RoutingId,
    call: ZLinkSpotAddressCallOptions
  ): MissingTargetSelection {
    const configuredMeshes = this.options.meshNames();
    if (
      call.initialMeshName !== undefined &&
      this.options.isMeshConfigured?.(call.initialMeshName) === false
    ) {
      throw createInternalFrameworkException(
        ZLinkFrameworkInternalErrorKind.MeshNotFound,
        `RouteMesh '${call.initialMeshName}' is not configured.`
      );
    }
    if (call.initialMeshName !== undefined && !configuredMeshes.includes(call.initialMeshName)) {
      throw createInternalFrameworkException(
        ZLinkFrameworkInternalErrorKind.ObjectClientNotConfigured,
        `RouteMesh '${call.initialMeshName}' has no object-client role.`
      );
    }
    if (call.initialMeshName === undefined && configuredMeshes.length === 0) {
      throw createInternalFrameworkException(
        ZLinkFrameworkInternalErrorKind.ObjectClientNotConfigured,
        'No object-client RouteMesh is configured.'
      );
    }
    if (call.initialMeshName === undefined && configuredMeshes.length > 1) {
      throw createInternalFrameworkException(
        ZLinkFrameworkInternalErrorKind.MeshSelectionRequired,
        'Multiple object-client RouteMeshes are configured; call inMesh(...).'
      );
    }
    const meshNames =
      call.initialMeshName === undefined ? configuredMeshes : [call.initialMeshName];
    const distinctTypes = [
      ...new Set(
        meshNames.flatMap(
          (meshName) => this.options.meshNode(meshName)?.instanceSpotPlacementTypes?.() ?? []
        )
      )
    ];
    const canInspectPlacementTypes = meshNames.some(
      (meshName) =>
        typeof this.options.meshNode(meshName)?.instanceSpotPlacementTypes === 'function'
    );
    const stableType =
      call.instanceSpotType ?? (distinctTypes.length === 1 ? distinctTypes[0] : undefined);
    if (stableType === undefined) {
      if (distinctTypes.length === 0) return { kind: 'unsupported' };
      throw createInternalFrameworkException(
        ZLinkFrameworkInternalErrorKind.InvalidConfiguration,
        'Instance Spot type is required when multiple types are registered.'
      );
    }
    if (canInspectPlacementTypes && !distinctTypes.includes(stableType)) {
      return { kind: 'unsupported' };
    }
    let unavailable = false;
    let capacity = false;
    let unsupported = false;
    for (const meshName of meshNames) {
      const node = this.options.meshNode(meshName);
      const placement = node?.selectObjectPlacement(stableType);
      if (node === undefined || placement === undefined) {
        unavailable = true;
        continue;
      }
      if (placement.kind === 'selected') {
        return {
          kind: 'selected',
          meshName,
          node,
          target: {
            ...placement.target,
            targetSpotId: String(spotId),
            stableType
          }
        };
      }
      if (placement.kind === 'unavailable') unavailable = true;
      if (placement.kind === 'capacity') capacity = true;
      if (placement.kind === 'unsupported') unsupported = true;
    }
    if (unavailable) return { kind: 'unavailable' };
    if (capacity) return { kind: 'capacity' };
    if (unsupported) return { kind: 'unsupported' };
    return { kind: 'unavailable' };
  }

  private validateExisting(
    target: import('../spots/spot-routing-internal').ZLinkSpotRouteTarget,
    call: ZLinkSpotAddressCallOptions
  ): void {
    if (!call.instanceSpot) return;
    if (
      target.spotKind !== ZLinkSpotKind.Instance ||
      (call.instanceSpotType !== undefined && target.stableType !== call.instanceSpotType)
    ) {
      throw createInternalFrameworkException(
        ZLinkFrameworkInternalErrorKind.SpotTypeMismatch,
        `Spot '${String(target.spotId)}' is not the requested Instance Spot type.`
      );
    }
  }

  private encode(
    kind: ZLinkChannelMessageKind,
    meshName: string,
    payload: unknown,
    timeoutMs?: number
  ): readonly MessageLike[] {
    return encodeChannelEnvelopeParts(
      kind,
      meshName,
      undefined,
      payload,
      timeoutMs,
      undefined,
      this.options.codecs,
      undefined,
      true,
      new Map()
    );
  }

  private encodeAtDeadline(
    kind: ZLinkChannelMessageKind,
    meshName: string,
    payload: unknown,
    deadlineUnixMs: number
  ): readonly MessageLike[] {
    return encodeChannelEnvelopePartsAtDeadline(
      kind,
      meshName,
      undefined,
      payload,
      deadlineUnixMs,
      undefined,
      this.options.codecs,
      undefined,
      true,
      new Map()
    );
  }
}

function missingInstanceRequestFailure(
  result: number,
  nativeErrno: number
): ZLinkFrameworkException {
  const canonical = isCanonicalWireReplyTerminal(result, nativeErrno);
  const wireKind = canonical
    ? internalFrameworkErrorKindFromWireReply(result, nativeErrno)
    : undefined;
  const kind = !canonical
    ? ZLinkFrameworkInternalErrorKind.RequestProtocolError
    : result === RequestResult.NotFound
      ? ZLinkFrameworkInternalErrorKind.RequestTargetNotFound
      : result === RequestResult.TimedOut
        ? ZLinkFrameworkInternalErrorKind.DeadlineExceeded
        : result === RequestResult.Terminated
          ? ZLinkFrameworkInternalErrorKind.RuntimeShutdown
          : result === RequestResult.Backpressured
            ? //  Spec 32-framework-error-model:104-108 — the bounded admission
              //  terminal is target placement capacity: Unavailable.
              ZLinkFrameworkInternalErrorKind.PlacementCapacityExhausted
            : result === RequestResult.NotConnected
              ? ZLinkFrameworkInternalErrorKind.RouteNotConnected
              : (wireKind ?? ZLinkFrameworkInternalErrorKind.RequestFailed);
  return createInternalFrameworkException(
    kind,
    `Instance Spot request failed with result ${result} and errno ${nativeErrno}.`
  );
}

function missingInstancePlacementCapacity(
  spotId: RoutingId,
  stableType: string | undefined
): ZLinkFrameworkException {
  const type = stableType === undefined ? '' : ` of type '${stableType}'`;
  return createInternalFrameworkException(
    ZLinkFrameworkInternalErrorKind.PlacementCapacityExhausted,
    `No eligible Instance Spot placement target${type} is available for '${String(spotId)}'.`,
    true
  );
}

function missingInstancePlacementUnavailable(
  spotId: RoutingId,
  stableType: string | undefined
): ZLinkFrameworkException {
  const type = stableType === undefined ? '' : ` of type '${stableType}'`;
  return createInternalFrameworkException(
    ZLinkFrameworkInternalErrorKind.RouteNotConnected,
    `No connected Instance Spot placement target${type} is available for '${String(spotId)}'.`,
    true
  );
}

function isSpotRouteRefreshError(error: unknown): error is ZLinkFrameworkException {
  if (!(error instanceof ZLinkFrameworkException)) return false;
  const kind = internalFrameworkErrorKind(error);
  return (
    kind === ZLinkFrameworkInternalErrorKind.SpotRouteNotFound ||
    kind === ZLinkFrameworkInternalErrorKind.SpotGenerationStale ||
    kind === ZLinkFrameworkInternalErrorKind.SpotMoving ||
    kind === ZLinkFrameworkInternalErrorKind.RequestTargetNotFound ||
    kind === ZLinkFrameworkInternalErrorKind.ActorLocationStale ||
    kind === ZLinkFrameworkInternalErrorKind.RouteNotConnected
  );
}

function isOwnerFenceRefusal(error: unknown): boolean {
  return (
    error instanceof ZLinkFrameworkException &&
    internalFrameworkErrorKind(error) === ZLinkFrameworkInternalErrorKind.SpotMoving
  );
}

function submitResultReason(status: ZLinkSubmitStatus): ZLinkDispatchErrorReason {
  switch (status) {
    case ZLinkSubmitStatus.Backpressured:
    case ZLinkSubmitStatus.TimedOut:
      return ZLinkDispatchErrorReason.Backpressure;
    case ZLinkSubmitStatus.Shutdown:
      return ZLinkDispatchErrorReason.Shutdown;
    case ZLinkSubmitStatus.TargetNotFound:
    case ZLinkSubmitStatus.RouteNotConnected:
      return ZLinkDispatchErrorReason.StaleTarget;
    case ZLinkSubmitStatus.Submitted:
      return ZLinkDispatchErrorReason.HandlerException;
  }
}

function submitResultFlowOutcome(status: ZLinkSubmitStatus): ZLinkMessageFlowOutcome {
  switch (status) {
    case ZLinkSubmitStatus.Submitted:
      return ZLinkMessageFlowOutcome.Sent;
    case ZLinkSubmitStatus.Backpressured:
    case ZLinkSubmitStatus.TimedOut:
      return ZLinkMessageFlowOutcome.Backpressured;
    case ZLinkSubmitStatus.Shutdown:
    case ZLinkSubmitStatus.TargetNotFound:
    case ZLinkSubmitStatus.RouteNotConnected:
      return ZLinkMessageFlowOutcome.Dropped;
  }
}

function addressedInstanceErrorReason(error: unknown): ZLinkDispatchErrorReason {
  if (error instanceof ZLinkFrameworkException) {
    const kind = internalFrameworkErrorKind(error);
    if (
      kind === ZLinkFrameworkInternalErrorKind.SpotRouteNotFound ||
      kind === ZLinkFrameworkInternalErrorKind.SpotGenerationStale ||
      kind === ZLinkFrameworkInternalErrorKind.SpotMoving ||
      kind === ZLinkFrameworkInternalErrorKind.RequestTargetNotFound ||
      kind === ZLinkFrameworkInternalErrorKind.ActorLocationStale ||
      kind === ZLinkFrameworkInternalErrorKind.RouteNotConnected
    ) {
      return ZLinkDispatchErrorReason.StaleTarget;
    }
    if (kind === ZLinkFrameworkInternalErrorKind.RuntimeShutdown) {
      return ZLinkDispatchErrorReason.Shutdown;
    }
    if (
      kind === ZLinkFrameworkInternalErrorKind.PlacementCapacityExhausted ||
      kind === ZLinkFrameworkInternalErrorKind.WorkerQueueFull ||
      kind === ZLinkFrameworkInternalErrorKind.WorkerTimedOut ||
      kind === ZLinkFrameworkInternalErrorKind.DeadlineExceeded
    ) {
      return ZLinkDispatchErrorReason.Backpressure;
    }
  }
  return ZLinkDispatchErrorReason.HandlerException;
}

interface ZLinkSpotAddressDeadline {
  readonly deadlineUnixMs: number;
  readonly signal: AbortSignal;
  requireRemaining(): number;
  expired(): boolean;
  close(): void;
}

function createSpotAddressDeadline(
  timeoutMs: number,
  parent?: AbortSignal
): ZLinkSpotAddressDeadline {
  const startedAtMs = performance.now();
  const startedAtUnixMs = Date.now();
  const deadlineMs = startedAtMs + Math.max(0, timeoutMs);
  const controller = new AbortController();
  let expired = false;
  let timeout: ReturnType<typeof setTimeout> | undefined;
  const expire = () => {
    expired = true;
    controller.abort();
  };
  const arm = () => {
    if (timeout !== undefined) clearTimeout(timeout);
    const remainingMs = deadlineMs - performance.now();
    if (remainingMs <= 0) {
      expire();
      return;
    }
    timeout = setTimeout(expire, remainingMs);
  };
  arm();
  const abort = () => controller.abort(parent?.reason);
  if (parent?.aborted === true) abort();
  else parent?.addEventListener('abort', abort, { once: true });
  return {
    get deadlineUnixMs() {
      return Math.round(startedAtUnixMs + (deadlineMs - startedAtMs));
    },
    signal: controller.signal,
    requireRemaining() {
      const remainingMs = deadlineMs - performance.now();
      if (remainingMs <= 0) {
        expired = true;
        throw createInternalFrameworkException(
          ZLinkFrameworkInternalErrorKind.DeadlineExceeded,
          'Spot address operation exceeded its end-to-end deadline.',
          true
        );
      }
      return Math.max(1, Math.ceil(remainingMs));
    },
    expired: () => expired,
    close() {
      if (timeout !== undefined) clearTimeout(timeout);
      parent?.removeEventListener('abort', abort);
    }
  };
}
