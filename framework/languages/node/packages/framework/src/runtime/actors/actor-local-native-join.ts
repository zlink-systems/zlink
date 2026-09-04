import {
  ZLinkFrameworkInternalErrorKind,
  createInternalFrameworkException,
  wireReplyFailureException
} from '../framework-errors-internal';
import { randomUUID } from 'node:crypto';
import { isBackendNotConnectedError } from '../backend/runtime-values';
import type {
  RoutingId,
  ZLinkActor,
  ZLinkActorJoinOperationId,
  ZLinkMessageSerializer,
} from '../../contracts';
import { ZLinkFrameworkErrorKind, ZLinkSpotKind } from '../../contracts';
import type { ZLinkActorJoinRuntimeResult } from './actor-runtime-contracts';
import type { Message } from '../../contracts/Common/Message';
import type {
  ZLinkBackendActorRef,
  ZLinkBackendMeshNode
} from '../backend/contracts';
import {
  closeMeshCompletion,
  type ZLinkMeshCompletion,
  type ZLinkMeshCompletionTable
} from '../backend/mesh-completion-table';
import { createAbortError, throwIfAborted } from '../abort';
import type { ZLinkSpotRouteTarget } from '../spots/spot-routing-internal';
import {
  ZLinkActorRuntimeState,
  toFrameworkActorRef,
  toFrameworkRoutingId
} from './actor-runtime-state';
import type { ZLinkPostCommitActorBinder } from './post-commit-actor-binder';
import type { ZLinkPostCommitActorLocation } from './post-commit-actor-location';
import { toBackendRoutingId as toBackendRoutingId } from '../routing-id';
import { routingIdsEqual } from '../routing-id';
import { operationIdentityKey } from '../foundation/operation-identity';
import { frameworkPayloadContentType } from '../messaging/payload-codec';
import type { ZLinkActorJoinRelocation } from './actor-join-relocation';

const ZLINK_FRAMEWORK_ACTOR_JOIN_PACKET_NAME = 'ZLinkFrameworkActorJoinRequest';

export interface ZLinkLocalNativeActorJoinOptions {
  readonly postCommitLocation?: ZLinkPostCommitActorLocation;
  readonly postCommitBinder?: ZLinkPostCommitActorBinder;
  readonly completionTableProvider: () => ZLinkMeshCompletionTable | undefined;
  readonly actorJoinRelocation?: ZLinkActorJoinRelocation;
  readonly messageSerializers?: ReadonlyMap<string, ZLinkMessageSerializer>;
}

/** Owns core-native actor joins and their local runtime state transition. */
export class ZLinkLocalNativeActorJoin {
  constructor(private readonly options: ZLinkLocalNativeActorJoinOptions) {}

  async joinSpot(
    node: ZLinkBackendMeshNode,
    actor: ZLinkActor,
    state: ZLinkActorRuntimeState,
    actorRef: ZLinkBackendActorRef,
    spotId: RoutingId,
    spotRouteTarget: ZLinkSpotRouteTarget | undefined,
    request: Message,
    timeoutMs: number | undefined,
    signal: AbortSignal | undefined,
    completionOperationId?: ZLinkActorJoinOperationId
  ): Promise<ZLinkActorJoinRuntimeResult<Message>> {
    if (signal?.aborted === true) throw createAbortError();
    const completions = this.requireCompletions();
    const target = requireUserSpotRoute(spotRouteTarget, spotId);
    const actorMeshName = runtimeActorMeshName(actor, state, target.routerChannelId);
    const remote = !routingIdsEqual(
      toFrameworkRoutingId(node.status().routingId),
      target.targetNodeRid
    );
    if (remote) {
      return await this.relocateRemoteActorJoin(
        node,
        actor,
        state,
        actorRef,
        target,
        request,
        timeoutMs,
        signal,
        completionOperationId,
        false
      );
    }
    const completion = await this.waitForJoinCompletion(
      () => node.joinActorSpot(
        actorRef,
        toBackendRoutingId(target.targetNodeRid),
        toBackendRoutingId(target.spotId),
        target.targetSpotGeneration,
        actorJoinApplicationPayload(request),
        timeoutMs
      ),
      completions,
      timeoutMs,
      signal
    );
    const control = completion.kindData;
    if (
      control?.kind !== 'actorJoinCompletion' ||
      control.actor === null
    ) {
      closeMeshCompletion(completion);
      const message = `Actor join failed for '${actor.context.actorId}' with result '${completion.terminalResult}' and errno '${completion.failureErrno}'.`;
      //  Classify the (terminal, fine) pair instead of collapsing every non-OK
      //  or malformed join completion to NotFound (spec 32-framework-error-model:
      //  83-118). An OK terminal that carries no join control is a protocol
      //  violation, not a missing route.
      throw completion.terminalResult !== 0 || completion.failureErrno !== 0
        ? wireReplyFailureException(
            completion.terminalResult,
            completion.failureErrno,
            message
          )
        : createInternalFrameworkException(
            ZLinkFrameworkInternalErrorKind.RequestProtocolError,
            message
          );
    }
    if (control.joinResult !== 0) {
      try {
        return {
          accepted: false,
          actor: toFrameworkActorRef(control.actor as never, actorMeshName),
          reply: completion.parts[0]
        };
      } finally {
        disposeParts(completion.parts.slice(1));
      }
    }
    if (completion.terminalResult !== 0 || completion.failureErrno !== 0) {
      closeMeshCompletion(completion);
      throw wireReplyFailureException(
        completion.terminalResult,
        completion.failureErrno,
        `Actor join failed for '${actor.context.actorId}' with result '${completion.terminalResult}' and errno '${completion.failureErrno}'.`
      );
    }

    state.setNativeActorRef(control.actor as never);
    state.setJoinedSpot(
      toFrameworkRoutingId(control.location.spotId ?? target.spotId),
      undefined,
      control.location.membershipEpoch,
      control.location.spotGeneration
    );
    // The Session owner relays from the verified Ready authority snapshot;
    // membership coordinates alone cannot recreate a User/Instance fence.
    state.setRemoteActorPacketTarget(
      target.spotKind === ZLinkSpotKind.Entry ? undefined : target
    );
    if (state.actorType !== undefined) {
      this.options.postCommitLocation?.joinedEventually(
        state.actorType,
        actor.context.actorId,
        spotRouteTarget?.routerChannelId ?? '',
        toFrameworkRoutingId(control.location.spotId ?? target.spotId),
        control.location.spotGeneration,
        control.location.membershipEpoch,
        node.status().lifecycleGeneration
      );
    }
    await this.options.postCommitBinder?.bind(
      toFrameworkActorRef(control.actor as never, actorMeshName)
    );
    try {
      return {
        accepted: true,
        actor: toFrameworkActorRef(
          control.actor as never,
          actorMeshName
        ),
        reply: completion.parts[0]
      };
    } finally {
      disposeParts(completion.parts.slice(1));
    }
  }

  async joinEntrySpot(
    node: ZLinkBackendMeshNode,
    actor: ZLinkActor,
    state: ZLinkActorRuntimeState,
    actorRef: ZLinkBackendActorRef,
    nodeRid: RoutingId,
    spotRouteTarget: ZLinkSpotRouteTarget | undefined,
    request: Message,
    timeoutMs: number | undefined,
    signal: AbortSignal | undefined,
    completionOperationId?: ZLinkActorJoinOperationId
  ): Promise<ZLinkActorJoinRuntimeResult<Message>> {
    if (signal?.aborted === true) throw createAbortError();
    const completions = this.requireCompletions();
    const actorMeshName = runtimeActorMeshName(actor, state, '');
    const targetNodeRid = spotRouteTarget?.targetNodeRid ?? nodeRid;
    const remote = spotRouteTarget !== undefined && (
      !routingIdsEqual(
        toFrameworkRoutingId(node.status().routingId),
        spotRouteTarget.targetNodeRid
      ) || !routingIdsEqual(
        toFrameworkRoutingId(actorRef.nodeRid),
        spotRouteTarget.targetNodeRid
      )
    );
    if (remote) {
      return await this.relocateRemoteActorJoin(
        node,
        actor,
        state,
        actorRef,
        spotRouteTarget!,
        request,
        timeoutMs,
        signal,
        completionOperationId,
        true
      );
    }
    const completion = await this.waitForJoinCompletion(
      () => node.joinActorEntrySpot(
        actorRef,
        toBackendRoutingId(targetNodeRid),
        actorJoinApplicationPayload(request),
        timeoutMs
      ),
      completions,
      timeoutMs,
      signal
    );
    const control = completion.kindData;
    if (
      control?.kind !== 'actorJoinCompletion' ||
      control.actor === null
    ) {
      closeMeshCompletion(completion);
      const message = `Actor entry SPOT join failed for '${actor.context.actorId}' with result '${completion.terminalResult}' and errno '${completion.failureErrno}'.`;
      //  Classify the (terminal, fine) pair instead of collapsing to NotFound
      //  (spec 32-framework-error-model:83-118); an OK terminal with no join
      //  control is a protocol violation.
      throw completion.terminalResult !== 0 || completion.failureErrno !== 0
        ? wireReplyFailureException(
            completion.terminalResult,
            completion.failureErrno,
            message
          )
        : createInternalFrameworkException(
            ZLinkFrameworkInternalErrorKind.RequestProtocolError,
            message
          );
    }
    if (control.joinResult !== 0) {
      try {
        return {
          accepted: false,
          actor: toFrameworkActorRef(control.actor as never, actorMeshName),
          reply: completion.parts[0]
        };
      } finally {
        disposeParts(completion.parts.slice(1));
      }
    }
    if (completion.terminalResult !== 0 || completion.failureErrno !== 0) {
      closeMeshCompletion(completion);
      throw wireReplyFailureException(
        completion.terminalResult,
        completion.failureErrno,
        `Actor entry SPOT join failed for '${actor.context.actorId}' with result '${completion.terminalResult}' and errno '${completion.failureErrno}'.`
      );
    }

    state.setNativeActorRef(control.actor as never);
    state.clearJoinedSpot();
    state.setRemoteActorPacketTarget(undefined);
    if (state.actorType !== undefined) {
      this.options.postCommitLocation?.leftEventually(
        state.actorType,
        actor.context.actorId,
        toFrameworkRoutingId(control.location.spotId ?? spotRouteTarget?.spotId ?? nodeRid),
        control.location.spotGeneration,
        control.location.membershipEpoch,
        node.status().lifecycleGeneration
      );
    }
    await this.options.postCommitBinder?.bind(
      toFrameworkActorRef(control.actor as never, actorMeshName)
    );
    try {
      return {
        accepted: true,
        actor: toFrameworkActorRef(
          control.actor as never,
          actorMeshName
        ),
        reply: completion.parts[0]
      };
    } finally {
      disposeParts(completion.parts.slice(1));
    }
  }

  private async waitForJoinCompletion(
    submit: () => { readonly high: bigint; readonly low: bigint },
    completions: ZLinkMeshCompletionTable,
    timeoutMs: number | undefined,
    signal: AbortSignal | undefined
  ): Promise<ZLinkMeshCompletion> {
    // The operation ID identifies one Core request. Once it is submitted, a
    // NotConnected completion is terminal for that request; resubmitting it
    // could execute the target lifecycle twice after a delayed reply.
    return submitJoinWhenConnected(
      () => completions.submit(submit, signal),
      timeoutMs,
      signal
    );
  }

  private async relocateRemoteActorJoin(
    node: ZLinkBackendMeshNode,
    actor: ZLinkActor,
    state: ZLinkActorRuntimeState,
    actorRef: ZLinkBackendActorRef,
    target: ZLinkSpotRouteTarget,
    request: Message,
    timeoutMs: number | undefined,
    signal: AbortSignal | undefined,
    completionOperationId: ZLinkActorJoinOperationId | undefined,
    entrySpot: boolean
  ): Promise<ZLinkActorJoinRuntimeResult<Message>> {
    const actorType = state.actorType;
    if (actorType === undefined) {
      throw createInternalFrameworkException(
        ZLinkFrameworkInternalErrorKind.ActorRouteNotFound,
        `Actor '${actor.context.actorId}' canonical remote Join identity is not configured.`
      );
    }
    const completions = this.requireCompletions();
    let relocationId: string = randomUUID();
    const completionOperationKey = completionOperationId === undefined
      ? undefined
      : operationIdentityKey(completionOperationId);
    if (completionOperationKey === relocationId) {
      throw new Error('Actor Join OperationId must be distinct from RelocationId.');
    }
    const actorAuthorityFence = remoteJoinAuthorityFence(node, state);
    // Command 28 is usable only when both the source Actor authority fence
    // and the backend's canonical entry point are present.  Until then keep
    // the existing internal admission route (wire protocol §10); fabricating
    // an Actor fence from the destination Spot would let a stale source pass
    // receiver-side Authority-row equality.
    const canonicalAdmission = actorAuthorityFence === undefined
      || !supportsCanonicalActorJoin(node, entrySpot)
      ? undefined
      : {
          request: actorJoinApplicationPayload(request),
          actorFence: {
            targetNodeGeneration: actorAuthorityFence.nodeGeneration,
            authorityOwnerGeneration: actorAuthorityFence.authorityOwnerGeneration,
            ownerLeaseGeneration: actorAuthorityFence.ownerLeaseGeneration
          },
          local: { phase: 'admission', transferId: relocationId } as const
        };
    let admissionOperationId: ZLinkActorJoinOperationId | undefined;
    const admission = await this.waitForJoinCompletion(
      () => {
        const legacyAdmission = canonicalAdmission === undefined
          ? legacyRemoteActorJoinPayload(actor, state, actorRef, target, request, relocationId)
          : undefined;
        const operationId = canonicalAdmission === undefined
          ? entrySpot
            ? node.joinActorEntrySpot(
                actorRef,
                toBackendRoutingId(target.targetNodeRid),
                legacyAdmission!,
                timeoutMs
              )
            : node.joinActorSpot(
                actorRef,
                toBackendRoutingId(target.targetNodeRid),
                toBackendRoutingId(target.spotId),
                target.targetSpotGeneration!,
                legacyAdmission!,
                timeoutMs
              )
          : entrySpot
            ? node.joinActorEntrySpotCanonical!(
                actorRef,
                toBackendRoutingId(target.targetNodeRid),
                canonicalAdmission.request,
                canonicalAdmission.actorFence,
                canonicalAdmission.local,
                timeoutMs
              )
            : node.joinActorSpotCanonical!(
                actorRef,
                toBackendRoutingId(target.targetNodeRid),
                toBackendRoutingId(target.spotId),
                target.targetSpotGeneration!,
                canonicalAdmission.request,
                canonicalAdmission.actorFence,
                canonicalAdmission.local,
                timeoutMs
              );
        admissionOperationId = operationId;
        return operationId;
      },
      completions,
      timeoutMs,
      signal
    );
    const control = admission.kindData;
    if (control?.kind !== 'actorJoinCompletion' || control.actor === null) {
      closeMeshCompletion(admission);
      const message = `Actor admission failed for '${actor.context.actorId}' with result '${admission.terminalResult}' and errno '${admission.failureErrno}'.`;
      //  Classify the (terminal, fine) pair instead of collapsing to NotFound
      //  (spec 32-framework-error-model:83-118); an OK terminal with no join
      //  control is a protocol violation.
      throw admission.terminalResult !== 0 || admission.failureErrno !== 0
        ? remoteActorJoinFailureException(
            node,
            target,
            admission.terminalResult,
            admission.failureErrno,
            message
          )
        : createInternalFrameworkException(
            ZLinkFrameworkInternalErrorKind.RequestProtocolError,
            message
          );
    }
    if (control.joinResult !== 0) {
      try {
        return {
          accepted: false,
          actor: toFrameworkActorRef(
            control.actor as never,
            runtimeActorMeshName(actor, state, target.routerChannelId)
          ),
          reply: admission.parts[0]
        };
      } finally {
        disposeParts(admission.parts.slice(1));
      }
    }
    if (admission.terminalResult !== 0 || admission.failureErrno !== 0) {
      closeMeshCompletion(admission);
      throw remoteActorJoinFailureException(
        node,
        target,
        admission.terminalResult,
        admission.failureErrno,
        `Actor admission failed for '${actor.context.actorId}' with result '${admission.terminalResult}' and errno '${admission.failureErrno}'.`
      );
    }
    disposeParts(admission.parts.slice(1));
    if (control.canonicalHandoffId !== undefined) {
      // Command 28 derives the canonical handoff identity from its authenticated
      // source fence and correlation. Direct recovery requires that exact value
      // to own the relocation aggregate as well; the public OperationId remains
      // a separate request-completion identity.
      relocationId = canonicalHandoffRelocationId(control.canonicalHandoffId);
    }
    const relocation = this.options.actorJoinRelocation;
    if (relocation === undefined) {
      admission.parts[0]?.close();
      throw createInternalFrameworkException(
        ZLinkFrameworkInternalErrorKind.ActorRouteNotFound,
        `Actor '${actor.context.actorId}' canonical remote Join relocation is not configured.`
      );
    }
    let relocated;
    try {
      if (completionOperationKey === relocationId) {
        throw new Error('Actor Join OperationId must be distinct from RelocationId.');
      }
      relocated = await relocation.relocateActorJoin({
        meshName: runtimeActorMeshName(actor, state, target.routerChannelId),
        actor,
        state,
        target,
        relocationId,
        completionOperationId,
        ...(control.canonicalHandoffId === undefined || canonicalAdmission === undefined
          ? {}
          : {
              canonicalRecovery: {
                handoffId: control.canonicalHandoffId,
                admissionOperationId: {
                  // Command 28 authenticates its operation as
                  // (source node generation, Core request sequence).
                  high: actorAuthorityFence!.nodeGeneration,
                  low: admissionOperationId!.low
                },
                requestContentType: canonicalAdmission.request.contentType,
                request: Buffer.from(canonicalAdmission.request.payload),
                ...(completionOperationId === undefined
                  ? {}
                  : {
                      replyContentType: control.replyContentType ?? 'application/octet-stream'
                    }),
                reply: completionOperationId === undefined
                  ? Buffer.alloc(0)
                  : Buffer.from(admission.parts[0]?.data() ?? []),
                actorNodeGeneration: actorAuthorityFence!.nodeGeneration,
                expectedOwnerLeaseGeneration: actorAuthorityFence!.ownerLeaseGeneration,
                targetNodeGeneration: target.targetNodeGeneration!,
                targetSpotGeneration: control.location.spotGeneration,
                targetAuthorityOwnerGeneration:
                  actorAuthorityFence!.authorityOwnerGeneration + 1n,
                targetSpotAuthorityOwnerGeneration:
                  target.authorityOwnerGeneration ?? 1n
              }
            }),
        advertisedReceiveChunkLimitBytes: control.receiveChunkLimitBytes,
        signal
      });
    } catch (error) {
      admission.parts[0]?.close();
      throw error;
    }
    state.setNativeActorRef(relocated.actorRef);
    if (entrySpot) {
      state.clearJoinedSpot();
      state.setRemoteActorPacketTarget(undefined);
    } else {
      state.setJoinedSpot(
        target.spotId,
        undefined,
        relocated.membershipEpoch,
        relocated.spotGeneration
      );
      state.setRemoteActorPacketTarget(target);
    }
    const targetActorRef = toFrameworkActorRef(
      relocated.actorRef,
      runtimeActorMeshName(actor, state, target.routerChannelId)
    );
    await this.options.postCommitBinder?.bind(targetActorRef);
    return {
      accepted: true,
      actor: targetActorRef,
      reply: admission.parts[0]
    };
  }

  private requireCompletions(): ZLinkMeshCompletionTable {
    const completions = this.options.completionTableProvider();
    if (completions === undefined) {
      throw createInternalFrameworkException(
        ZLinkFrameworkInternalErrorKind.ActorRouteNotFound,
        'Actor join runtime is not started.'
      );
    }
    return completions;
  }

}

function remoteJoinAuthorityFence(
  node: ZLinkBackendMeshNode,
  state: ZLinkActorRuntimeState
): {
  readonly nodeGeneration: bigint;
  readonly authorityOwnerGeneration: bigint;
  readonly ownerLeaseGeneration: bigint;
} | undefined {
  if (state.locationGeneration === undefined || state.ownerLeaseGeneration === undefined) {
    return undefined;
  }
  return {
    nodeGeneration: node.status().lifecycleGeneration,
    authorityOwnerGeneration: state.locationGeneration,
    ownerLeaseGeneration: state.ownerLeaseGeneration
  };
}

function supportsCanonicalActorJoin(
  node: ZLinkBackendMeshNode,
  entrySpot: boolean
): boolean {
  return entrySpot
    ? typeof node.joinActorEntrySpotCanonical === 'function'
    : typeof node.joinActorSpotCanonical === 'function';
}

function actorJoinApplicationPayload(request: Message) {
  return {
    packetName: ZLINK_FRAMEWORK_ACTOR_JOIN_PACKET_NAME,
    contentType: frameworkPayloadContentType(request),
    payload: Buffer.from(request.data())
  };
}

function legacyRemoteActorJoinPayload(
  actor: ZLinkActor,
  state: ZLinkActorRuntimeState,
  actorRef: ZLinkBackendActorRef,
  target: ZLinkSpotRouteTarget,
  request: Message,
  transferId: string
) {
  // The pre-command-28 route keeps its own JSON envelope.  It is selected
  // only when this backend cannot prove the source Actor fence needed for
  // canonical admission; it never substitutes target-Spot values as an Actor
  // authority fence.
  const payload = Buffer.from(JSON.stringify({
    packetName: '__zlink.actor.join_spot.request',
    phase: 'admission',
    transferId,
    spotId: String(target.spotId),
    actorId: actor.context.actorId,
    actorType: state.actorType,
    actorNodeRid: String(actorRef.nodeRid),
    actorGeneration: actorRef.generation.toString(),
    ...(state.spotId === undefined ? {} : { sourceSpotId: String(state.spotId) }),
    ...(target.routerChannelId.length === 0 ? {} : { routerChannelId: target.routerChannelId }),
    request: Buffer.from(request.data()).toString('base64'),
    requestContentType: frameworkPayloadContentType(request)
  }));
  return {
    packetName: ZLINK_FRAMEWORK_ACTOR_JOIN_PACKET_NAME,
    contentType: 'application/json',
    payload,
    // Older in-process MeshNode adapters consume the fallback as a Message.
    // Keep that structural view without changing the typed service payload.
    data: () => payload,
    getString: (encoding?: BufferEncoding) => payload.toString(encoding)
  };
}

function canonicalHandoffRelocationId(handoffId: string): string {
  const hex = handoffId.replaceAll('-', '').toLowerCase();
  if (!/^[0-9a-f]{32}$/u.test(hex) || /^0+$/u.test(hex)) {
    throw new TypeError('Canonical Actor Join handoff id is not a non-zero 128-bit identity.');
  }
  return `${hex.slice(0, 8)}-${hex.slice(8, 12)}-${hex.slice(12, 16)}`
    + `-${hex.slice(16, 20)}-${hex.slice(20)}`;
}

function remoteActorJoinFailureException(
  node: ZLinkBackendMeshNode,
  target: ZLinkSpotRouteTarget,
  terminalResult: number,
  failureErrno: number,
  message: string
) {
  const failure = wireReplyFailureException(terminalResult, failureErrno, message);
  const targetGeneration = target.targetNodeGeneration;
  if (
    failure.kind === ZLinkFrameworkErrorKind.DeadlineExceeded
    && targetGeneration !== undefined
    && targetGeneration !== 0n
    && !node.peers().some((peer) =>
      peer.state === 3
      && peer.routingId !== null
      && routingIdsEqual(
        peer.routingId,
        target.targetNodeRid
      )
      && peer.lifecycleGeneration === targetGeneration)
  ) {
    return createInternalFrameworkException(
      ZLinkFrameworkInternalErrorKind.RouteNotConnected,
      'wire Actor join target RouteMesh peer became unavailable'
    );
  }
  return failure;
}

function runtimeActorMeshName(
  actor: ZLinkActor,
  state: ZLinkActorRuntimeState,
  fallback: string
): string {
  const context = (
    actor as unknown as {
      readonly context?: { readonly meshName?: string };
    }
  ).context;
  const stateMeshName = (
    state as unknown as { readonly meshName?: string } | undefined
  )?.meshName;
  return context?.meshName ?? stateMeshName ?? fallback;
}

async function submitJoinWhenConnected<T>(
  submit: () => T,
  timeoutMs: number | undefined,
  signal: AbortSignal | undefined
): Promise<T> {
  const deadline = Date.now() + Math.min(timeoutMs ?? 5_000, 5_000);
  for (;;) {
    throwIfAborted(signal);
    try {
      return submit();
    } catch (error) {
      if (!isBackendNotConnectedError(error) || Date.now() >= deadline) {
        throw error;
      }
      await new Promise<void>((resolve) => setTimeout(resolve, 10));
    }
  }
}

function requireUserSpotRoute(
  target: ZLinkSpotRouteTarget | undefined,
  spotId: RoutingId
): ZLinkSpotRouteTarget & { readonly targetSpotGeneration: bigint } {
  if (target?.targetSpotGeneration === undefined || target.targetSpotGeneration <= 0n) {
    throw createInternalFrameworkException(
      ZLinkFrameworkInternalErrorKind.ActorRouteNotFound,
      `SPOT '${spotId}' has no valid Core lifecycle generation.`
    );
  }
  return target as ZLinkSpotRouteTarget & { readonly targetSpotGeneration: bigint };
}

function disposeParts(parts: readonly Message[]): void {
  for (const part of parts) {
    part.close();
  }
}
