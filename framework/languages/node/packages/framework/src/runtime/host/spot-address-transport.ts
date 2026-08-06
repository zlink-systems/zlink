import {
  ZLinkFrameworkInternalErrorKind,
  createInternalFrameworkException,
  internalFrameworkErrorKind,
  internalFrameworkErrorKindFromWireReply,
  isCanonicalWireReplyTerminal
} from '../framework-errors-internal';
import {
  RequestResult,
  SubmitResult,
  type ZLinkBackendMessageLike as MessageLike
} from '../backend/runtime-values';
import {
  ZLinkFrameworkException,
  type RoutingId
} from '../../contracts';
import {
  ZLinkSubmitStatus,
  type ZLinkSubmitResult
} from '../messaging/submission-result';
import {
  ZLinkSpotKind
} from '../../contracts';
import {
  ZLinkRuntimeMessageFlowOutcome as ZLinkMessageFlowOutcome,
  ZLinkRuntimeDispatchErrorAction as ZLinkDispatchErrorAction,
  ZLinkRuntimeDispatchErrorReason as ZLinkDispatchErrorReason,
  ZLinkDispatchErrorSurface,
  ZLinkDispatchMessageKind
} from '../../contracts/Dispatch/ZLinkDispatchOptions';
import {
  awaitWithAbort
} from '../abort';
import type { ZLinkBackendMeshNode } from '../backend/contracts';
import {
  closeMeshCompletion,
  type ZLinkMeshCompletionTable
} from '../backend/mesh-completion-table';
import {
  decodeChannelReply,
  encodeChannelEnvelopeParts,
  encodeChannelEnvelopePartsAtDeadline,
  type ZLinkChannelEnvelopeCodecRegistry,
  ZLinkChannelMessageKind
} from '../channels/channel-envelope';
import type { ZLinkDispatchErrorReporter } from '../channels';
import { flowIfEnabled } from '../diagnostics';
import { resolveFrameworkPacketName } from '../messaging/packet-name';
import type {
  ZLinkSpotAddressCallOptions,
  ZLinkSpotAddressTransport,
  ZLinkSpotRoutedTransport
} from '../spots/spot-outbound';
import type {
  ZLinkSpotRouteResolver,
  ZLinkSpotRouteTarget
} from '../spots/spot-routing-internal';

export interface ZLinkHostSpotAddressTransportOptions {
  readonly resolver: () => ZLinkSpotRouteResolver | undefined;
  readonly routed: ZLinkSpotRoutedTransport;
  readonly meshNames: () => readonly string[];
  readonly isMeshConfigured?: (meshName: string) => boolean;
  readonly meshNode: (meshName: string) => ZLinkBackendMeshNode | undefined;
  readonly completions: (meshName: string) => ZLinkMeshCompletionTable | undefined;
  readonly codecs?: ZLinkChannelEnvelopeCodecRegistry;
  readonly defaultRequestTimeoutMs: number;
  /** Internal one-way deadline policy; public calls do not expose this option. */
  readonly defaultSendTimeoutMs?: number;
  /** Resolves the send timeout for a configured Spot Mesh. */
  readonly sendTimeoutMsForMesh?: (meshName: string) => number;
  /** Resolves the send timeout for a resolved Spot route channel. */
  readonly sendTimeoutMsForRouteChannel?: (routeChannelId: string) => number;
  readonly dispatchErrors?: ZLinkDispatchErrorReporter;
  /** Internal admission hook; public Spot calls do not expose this transport. */
  readonly submitMissingInstance?: (
    meshName: string,
    attempt: () => ZLinkSubmitResult,
    signal: AbortSignal,
    timeoutMs: number
  ) => Promise<ZLinkSubmitResult>;
}

export function hasObjectClientCapability(
  role: 'none' | 'client' | 'server' | undefined
): boolean {
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

// Only a synchronous target-admission rejection is safe to retry. A
// completion with the same public error kind may already represent an
// admitted application operation, so it must not enter the retry loop.
const PRE_ADMISSION_MISSING_INSTANCE_ERRORS = new WeakSet<ZLinkFrameworkException>();

/** Owns global Spot authority lookup and Missing Instance placement. */
export class ZLinkHostSpotAddressTransport implements ZLinkSpotAddressTransport {
  constructor(private readonly options: ZLinkHostSpotAddressTransportOptions) {}

  async sendToSpotAddress(
    spotId: RoutingId,
    message: unknown,
    call: ZLinkSpotAddressCallOptions
  ): Promise<ZLinkSubmitResult> {
    const timeoutMs = initialSendTimeoutMs(this.options, call);
    const deadline = createSpotAddressDeadline(timeoutMs, call.signal);
    try {
    const existing = await this.resolveExisting(spotId, deadline.signal);
    deadline.requireRemaining();
    if (existing !== undefined) {
      this.validateExisting(existing, call);
      deadline.setOwnerTimeout(this.sendTimeoutMsForRouteChannel(existing.routerChannelId));
      try {
        const result = await awaitWithAbort(this.options.routed.sendToSpot(existing, message, {
          timeoutMs: deadline.requireRemaining(),
          signal: deadline.signal,
          metadata: call.metadata
        }), deadline.signal);
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
            ZLinkMessageFlowOutcome.Dropped,
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
          result.status === ZLinkSubmitStatus.TargetNotFound
          || result.status === ZLinkSubmitStatus.RouteNotConnected
        ) {
          this.options.resolver()?.invalidate?.(spotId);
        }
        return result;
      } catch (error) {
        if (isSpotRouteRefreshError(error)) {
          this.options.resolver()?.invalidate?.(spotId);
        }
        this.traceInstanceAddress(
          ZLinkMessageFlowOutcome.Dropped,
          ZLinkDispatchMessageKind.Send,
          spotId,
          message,
          existing.routerChannelId,
          existing.stableType,
          existing.targetNodeRid,
          addressedInstanceErrorReason(error)
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
        ZLinkMessageFlowOutcome.Dropped,
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
    deadline.setOwnerTimeout(this.sendTimeoutMsForMesh(selected.meshName));
    const encoded = this.encode(ZLinkChannelMessageKind.Command, selected.meshName, message);
    const sourceSpotId = call.sourceSpot === undefined
      ? undefined
      : String(call.sourceSpot.routingId);
    const prepared = selected.node.prepareMissingInstanceSpotSend?.(
      selected.target,
      encoded,
      BigInt(deadline.deadlineMs),
      sourceSpotId,
      call.metadata
    );
    const submit = prepared === undefined
      ? () => mapSubmitResult(selected.node.sendToMissingInstanceSpot(
          selected.target,
          encoded,
          BigInt(deadline.deadlineMs),
          sourceSpotId,
          call.metadata
        ))
      : () => mapSubmitResult(prepared());
    const mapped = this.options.submitMissingInstance === undefined
      ? submit()
      : await awaitWithAbort(
          this.options.submitMissingInstance(
            selected.meshName,
            submit,
            deadline.signal,
            deadline.requireRemaining()
          ),
          deadline.signal
        );
    this.traceInstanceAddress(
      mapped.status === ZLinkSubmitStatus.Submitted
        ? ZLinkMessageFlowOutcome.Sent
        : ZLinkMessageFlowOutcome.Dropped,
      ZLinkDispatchMessageKind.Send,
      spotId,
      message,
      selected.meshName,
      selected.target.stableType,
      selected.target.targetNodeRid,
      mapped.status === ZLinkSubmitStatus.Submitted ? undefined : submitResultReason(mapped.status)
    );
    return mapped;
    } catch (error) {
      if (deadline.expired()) {
        return { status: ZLinkSubmitStatus.TimedOut };
      }
      throw error;
    } finally {
      deadline.close();
    }
  }

  async requestToSpotAddress<TReply = unknown>(
    spotId: RoutingId,
    request: unknown,
    call: ZLinkSpotAddressCallOptions
  ): Promise<TReply> {
    const timeoutMs = call.timeoutMs ?? this.options.defaultRequestTimeoutMs;
    const deadline = createSpotAddressDeadline(timeoutMs, call.signal);
    try {
      for (;;) {
        const existing = await this.resolveExisting(spotId, deadline.signal);
        deadline.requireRemaining();

        if (existing !== undefined) {
          this.validateExisting(existing, call);
          try {
            return await this.requestToExistingSpot(spotId, request, call, existing, deadline);
          } catch (error) {
            if (isSpotRouteRefreshError(error)) {
              this.options.resolver()?.invalidate?.(spotId);
            }
            const recovery = await this.recoverStaleInstanceRoute(
              spotId,
              call,
              deadline,
              error,
              existing
            );
            if (recovery.kind === 'route') {
              // Retry only after the resolver exposes a different route. The
              // stale target cannot have admitted the application operation.
              continue;
            }
            if (recovery.kind !== 'cold') {
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
            // The logical authority is no longer Ready, so the Instance
            // intent path below may submit the original operation once.
          }
        }

        try {
          return await this.requestToMissingInstance(spotId, request, call, deadline);
        } catch (error) {
          if (!isMissingInstanceRetryError(error)) throw error;
          // A placement node can finish cleanup before its stale native result
          // reaches this process. Refresh authority and select again under the
          // same end-to-end deadline; the old envelope was not admitted.
          this.options.resolver()?.invalidate?.(spotId);
          await waitForSpotRouteRefresh(
            Math.min(10, deadline.requireRemaining()),
            deadline.signal
          );
        }
      }
    } catch (error) {
      if (
        deadline.expired()
        && !(
          error instanceof ZLinkFrameworkException
          && internalFrameworkErrorKind(error) === ZLinkFrameworkInternalErrorKind.RequestTargetNotFound
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
    const reply = await awaitWithAbort(this.options.routed.requestToSpot<TReply>(existing, request, {
      timeoutMs: deadline.requireRemaining(),
      signal: deadline.signal,
      metadata: call.metadata
    }), deadline.signal);
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
    if (selected.kind === 'capacity') {
      throw missingInstancePlacementCapacity(spotId, call.instanceSpotType);
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

    const deadlineUnixMs = BigInt(deadline.deadlineMs);
    const encoded = this.encodeAtDeadline(
      ZLinkChannelMessageKind.Request,
      selected.meshName,
      request,
      deadline.deadlineMs
    );
    deadline.requireRemaining();
    let operation: ReturnType<ZLinkBackendMeshNode['requestToMissingInstanceSpot']>;
    try {
      operation = selected.node.requestToMissingInstanceSpot(
        selected.target,
        encoded,
        deadlineUnixMs,
        call.sourceSpot === undefined ? undefined : String(call.sourceSpot.routingId),
        call.metadata
      );
    } catch (error) {
      if (
        error instanceof ZLinkFrameworkException
        && internalFrameworkErrorKind(error) === ZLinkFrameworkInternalErrorKind.RequestTargetNotFound
      ) {
        PRE_ADMISSION_MISSING_INSTANCE_ERRORS.add(error);
      }
      throw error;
    }
    this.traceInstanceAddress(
      ZLinkMessageFlowOutcome.Sent,
      ZLinkDispatchMessageKind.Request,
      spotId,
      request,
      selected.meshName,
      selected.target.stableType,
      selected.target.targetNodeRid
    );
    const table = this.options.completions(selected.meshName);
    if (table === undefined) {
      throw new Error(`MeshNode '${selected.meshName}' completion table is not started.`);
    }
    const completion = await awaitWithAbort(table.wait(operation, deadline.signal), deadline.signal);
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
        error instanceof ZLinkFrameworkException
        && internalFrameworkErrorKind(error) === ZLinkFrameworkInternalErrorKind.SpotRouteNotFound
      ) {
        return undefined;
      }
      throw error;
    }
  }

  private async recoverStaleInstanceRoute(
    spotId: RoutingId,
    call: ZLinkSpotAddressCallOptions,
    deadline: ZLinkSpotAddressDeadline,
    error: unknown,
    staleRoute: ZLinkSpotRouteTarget
  ): Promise<
    | { readonly kind: 'route'; readonly route: ZLinkSpotRouteTarget }
    | { readonly kind: 'cold' }
    | { readonly kind: 'fail' }
  > {
    if (
      !call.instanceSpot
      || !(error instanceof ZLinkFrameworkException)
      || !isInstanceRouteStaleError(error)
    ) {
      return { kind: 'fail' };
    }
    for (;;) {
      deadline.requireRemaining();
      const current = await this.resolveExisting(spotId, deadline.signal);
      if (current === undefined) return { kind: 'cold' };
      if (!sameSpotRouteSnapshot(current, staleRoute)) {
        return { kind: 'route', route: current };
      }
      await waitForSpotRouteRefresh(
        Math.min(10, deadline.requireRemaining()),
        deadline.signal
      );
    }
  }

  private selectMissingTarget(
    spotId: RoutingId,
    call: ZLinkSpotAddressCallOptions
  ): MissingTargetSelection {
    const configuredMeshes = this.options.meshNames();
    if (
      call.initialMeshName !== undefined
      && this.options.isMeshConfigured?.(call.initialMeshName) === false
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
    const meshNames = call.initialMeshName === undefined
      ? configuredMeshes
      : [call.initialMeshName];
    const distinctTypes = [...new Set(meshNames.flatMap(meshName =>
      this.options.meshNode(meshName)?.instanceSpotPlacementTypes?.() ?? []
    ))];
    const canInspectPlacementTypes = meshNames.some(meshName =>
      typeof this.options.meshNode(meshName)?.instanceSpotPlacementTypes === 'function'
    );
    const stableType = call.instanceSpotType
      ?? (distinctTypes.length === 1 ? distinctTypes[0] : undefined);
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

  private sendTimeoutMsForMesh(meshName: string): number {
    return this.options.sendTimeoutMsForMesh?.(meshName)
      ?? this.options.defaultSendTimeoutMs
      ?? this.options.defaultRequestTimeoutMs;
  }

  private sendTimeoutMsForRouteChannel(routeChannelId: string): number {
    return this.options.sendTimeoutMsForRouteChannel?.(routeChannelId)
      ?? this.options.defaultSendTimeoutMs
      ?? this.options.defaultRequestTimeoutMs;
  }

  private validateExisting(
    target: import('../spots/spot-routing-internal').ZLinkSpotRouteTarget,
    call: ZLinkSpotAddressCallOptions
  ): void {
    if (!call.instanceSpot) return;
    if (
      target.spotKind !== ZLinkSpotKind.Instance
      || (
        call.instanceSpotType !== undefined
        && target.stableType !== call.instanceSpotType
      )
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

function isInstanceRouteStaleError(error: unknown): error is ZLinkFrameworkException {
  if (!(error instanceof ZLinkFrameworkException)) return false;
  if ((error as ZLinkFrameworkException & { readonly physicalSubmission?: boolean })
    .physicalSubmission === true) return false;
  const kind = internalFrameworkErrorKind(error);
  // RequestTargetNotFound from an existing route is a pre-admission route
  // lookup failure. ActorLocationStale can be reported after the routed
  // request has crossed the transport boundary, so resubmitting the same
  // application operation could execute it twice.
  return kind === ZLinkFrameworkInternalErrorKind.RequestTargetNotFound;
}

function isMissingInstanceRetryError(error: unknown): error is ZLinkFrameworkException {
  return error instanceof ZLinkFrameworkException
    && PRE_ADMISSION_MISSING_INSTANCE_ERRORS.has(error);
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
          : result === RequestResult.NotConnected || result === RequestResult.Backpressured
            ? ZLinkFrameworkInternalErrorKind.RouteNotConnected
            : wireKind ?? ZLinkFrameworkInternalErrorKind.RequestFailed;
  return createInternalFrameworkException(
    kind,
    `Instance Spot request failed with result ${result} and errno ${nativeErrno}.`
  );
}

function mapSubmitResult(result: number): ZLinkSubmitResult {
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
        `Instance Spot submission failed with result ${result}.`
      );
  }
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
  return kind === ZLinkFrameworkInternalErrorKind.SpotRouteNotFound
    || kind === ZLinkFrameworkInternalErrorKind.SpotGenerationStale
    || kind === ZLinkFrameworkInternalErrorKind.SpotMoving
    || kind === ZLinkFrameworkInternalErrorKind.RequestTargetNotFound
    || kind === ZLinkFrameworkInternalErrorKind.ActorLocationStale
    || kind === ZLinkFrameworkInternalErrorKind.RouteNotConnected;
}

function sameSpotRouteSnapshot(
  left: ZLinkSpotRouteTarget,
  right: ZLinkSpotRouteTarget
): boolean {
  return String(left.targetNodeRid) === String(right.targetNodeRid)
    && String(left.spotId) === String(right.spotId)
    && left.routerChannelId === right.routerChannelId
    && left.spotKind === right.spotKind
    && left.stableType === right.stableType
    && left.targetSpotGeneration === right.targetSpotGeneration
    && left.targetNodeGeneration === right.targetNodeGeneration
    && left.authorityOwnerGeneration === right.authorityOwnerGeneration
    && left.targetOwnerId === right.targetOwnerId
    && left.ownerLeaseGeneration === right.ownerLeaseGeneration
    && left.authorityStoreVersion === right.authorityStoreVersion;
}

function waitForSpotRouteRefresh(delayMs: number, signal: AbortSignal): Promise<void> {
  return new Promise((resolve, reject) => {
    const finish = () => {
      signal.removeEventListener('abort', abort);
      resolve();
    };
    const abort = () => {
      clearTimeout(timer);
      signal.removeEventListener('abort', abort);
      reject(signal.reason);
    };
    const timer = setTimeout(finish, delayMs);
    signal.addEventListener('abort', abort, { once: true });
    if (signal.aborted) abort();
  });
}

function submitResultReason(
  status: ZLinkSubmitStatus
): ZLinkDispatchErrorReason {
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

function addressedInstanceErrorReason(error: unknown): ZLinkDispatchErrorReason {
  if (error instanceof ZLinkFrameworkException) {
    const kind = internalFrameworkErrorKind(error);
    if (
      kind === ZLinkFrameworkInternalErrorKind.SpotRouteNotFound
      || kind === ZLinkFrameworkInternalErrorKind.SpotGenerationStale
      || kind === ZLinkFrameworkInternalErrorKind.SpotMoving
      || kind === ZLinkFrameworkInternalErrorKind.RequestTargetNotFound
      || kind === ZLinkFrameworkInternalErrorKind.ActorLocationStale
      || kind === ZLinkFrameworkInternalErrorKind.RouteNotConnected
    ) {
      return ZLinkDispatchErrorReason.StaleTarget;
    }
    if (kind === ZLinkFrameworkInternalErrorKind.RuntimeShutdown) {
      return ZLinkDispatchErrorReason.Shutdown;
    }
  }
  return ZLinkDispatchErrorReason.HandlerException;
}

function initialSendTimeoutMs(
  options: ZLinkHostSpotAddressTransportOptions,
  call: ZLinkSpotAddressCallOptions
): number {
  return call.initialMeshName === undefined
    ? options.defaultSendTimeoutMs ?? options.defaultRequestTimeoutMs
    : options.sendTimeoutMsForMesh?.(call.initialMeshName)
      ?? options.defaultSendTimeoutMs
      ?? options.defaultRequestTimeoutMs;
}

interface ZLinkSpotAddressDeadline {
  readonly deadlineMs: number;
  readonly signal: AbortSignal;
  setOwnerTimeout(timeoutMs: number): void;
  requireRemaining(): number;
  expired(): boolean;
  close(): void;
}

function createSpotAddressDeadline(timeoutMs: number, parent?: AbortSignal): ZLinkSpotAddressDeadline {
  const startedAtMs = Date.now();
  let deadlineMs = startedAtMs + Math.max(0, timeoutMs);
  const controller = new AbortController();
  let expired = false;
  let timeout: ReturnType<typeof setTimeout> | undefined;
  const expire = () => {
    expired = true;
    controller.abort();
  };
  const arm = () => {
    if (timeout !== undefined) clearTimeout(timeout);
    const remainingMs = deadlineMs - Date.now();
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
    get deadlineMs() {
      return deadlineMs;
    },
    signal: controller.signal,
    setOwnerTimeout(ownerTimeoutMs: number) {
      deadlineMs = startedAtMs + Math.max(0, ownerTimeoutMs);
      arm();
    },
    requireRemaining() {
      const remainingMs = deadlineMs - Date.now();
      if (remainingMs <= 0) {
        expired = true;
        throw createInternalFrameworkException(
          ZLinkFrameworkInternalErrorKind.DeadlineExceeded,
          'Spot address operation exceeded its end-to-end deadline.',
          true
        );
      }
      return Math.max(1, remainingMs);
    },
    expired: () => expired,
    close() {
      if (timeout !== undefined) clearTimeout(timeout);
      parent?.removeEventListener('abort', abort);
    }
  };
}
