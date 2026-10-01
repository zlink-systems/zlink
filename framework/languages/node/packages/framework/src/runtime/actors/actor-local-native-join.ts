const LOCAL_JOIN_RESOLUTION_TIMEOUT_MS = 5_000;
const LOCAL_JOIN_POLL_INTERVAL_MS = 10;

import { ZlinkStreamContentType } from '@zlink-systems/stream-wire';
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
  ZLinkSpot,
  ZLinkSpotActorJoinResult
} from '../../contracts';
import { ZLinkSpotKind } from '../../contracts';
import type { ZLinkActorJoinRuntimeResult } from './actor-runtime-contracts';
import type { Message } from '../../contracts/Common/Message';
import type { ZLinkBackendActorRef, ZLinkBackendMeshNode } from '../backend/contracts';
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
import type { ZLinkLocationLifecycle } from '../locations';
import { toBackendRoutingId as toBackendRoutingId } from '../routing-id';
import { routingIdsEqual } from '../routing-id';
import { operationIdentityKey } from '../foundation/operation-identity';
import { frameworkPayloadContentType } from '../messaging/payload-codec';
import type { ZLinkActorJoinRelocation } from './actor-join-relocation';
import { ZLINK_REMOTE_ACTOR_JOIN_PACKET } from './actor-remote-wire';

const ZLINK_FRAMEWORK_ACTOR_JOIN_PACKET_NAME = 'ZLinkFrameworkActorJoinRequest';

export interface ZLinkLocalNativeActorJoinOptions {
  readonly locationLifecycle?: ZLinkLocationLifecycle;
  readonly localSpotJoin?: (
    spotId: RoutingId,
    actor: ZLinkActor,
    request: Message,
    commit: (spot: ZLinkSpot) => Promise<void>,
    signal?: AbortSignal,
    leaveSource?: () => Promise<void>,
    contentType?: string
  ) => Promise<ZLinkSpotActorJoinResult>;
  readonly localEntryJoin?: (actor: ZLinkActor, signal?: AbortSignal) => Promise<void>;
  readonly localSourceLeave?: (
    actor: ZLinkActor,
    spotId: RoutingId | undefined,
    signal?: AbortSignal
  ) => Promise<void>;
  readonly reportSourceLeaveError: (error: unknown) => void;
  readonly postCommitBinder?: ZLinkPostCommitActorBinder;
  readonly completionTableProvider: () => ZLinkMeshCompletionTable | undefined;
  readonly actorJoinRelocation?: ZLinkActorJoinRelocation;
  readonly messageSerializers?: ReadonlyMap<string, ZLinkMessageSerializer>;
}

/** Selects local membership commits or remote Actor relocation. */
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
    const target = requireUserSpotRoute(spotRouteTarget, spotId);
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
    return await this.joinLocal(node, actor, state, actorRef, target, request, signal);
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
    const targetNodeRid = spotRouteTarget?.targetNodeRid ?? nodeRid;
    const remote =
      spotRouteTarget !== undefined &&
      (!routingIdsEqual(
        toFrameworkRoutingId(node.status().routingId),
        spotRouteTarget.targetNodeRid
      ) ||
        !routingIdsEqual(toFrameworkRoutingId(actorRef.nodeRid), spotRouteTarget.targetNodeRid));
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
    const target = spotRouteTarget ?? {
      routerChannelId: runtimeActorMeshName(actor, state, ''),
      targetNodeRid,
      spotId: toFrameworkRoutingId(node.entrySpot().routingId),
      spotKind: ZLinkSpotKind.Entry,
      targetSpotGeneration: node.entrySpot().status().lifecycleGeneration
    };
    return await this.joinLocal(node, actor, state, actorRef, target, request, signal);
  }

  private async joinLocal(
    node: ZLinkBackendMeshNode,
    actor: ZLinkActor,
    state: ZLinkActorRuntimeState,
    actorRef: ZLinkBackendActorRef,
    target: ZLinkSpotRouteTarget,
    request: Message,
    signal: AbortSignal | undefined
  ): Promise<ZLinkActorJoinRuntimeResult<Message>> {
    const location = node.actorLookup(actor.context.actorId);
    const meshName = runtimeActorMeshName(actor, state, target.routerChannelId);
    if (routingIdsEqual(toFrameworkRoutingId(location.spotId), target.spotId)) {
      return { accepted: true, actor: toFrameworkActorRef(actorRef, meshName) };
    }
    if (state.actorType === undefined)
      throw new Error('Local Actor Join requires a registered Actor type.');
    const actorType = state.actorType;
    const previousSpotId = state.spotId;
    const spotGeneration = target.targetSpotGeneration;
    if (
      spotGeneration === undefined ||
      spotGeneration <= 0n ||
      node.restoreActorAuthority === undefined
    ) {
      throw new Error(
        'Local Actor Join requires a valid target generation and stateful membership runtime.'
      );
    }
    const membershipEpoch = location.membershipEpoch + 1n;
    const commit = async (spot?: ZLinkSpot): Promise<void> => {
      throwIfAborted(signal);
      if (target.spotKind === ZLinkSpotKind.Entry) {
        await this.options.locationLifecycle?.notifyActorLeftSpot(
          actorType,
          actor.context.actorId,
          target.spotId,
          spotGeneration,
          membershipEpoch,
          node.status().lifecycleGeneration
        );
      } else {
        await this.options.locationLifecycle?.notifyActorJoinedSpot(
          actorType,
          actor.context.actorId,
          meshName,
          target.spotId,
          spotGeneration,
          membershipEpoch,
          node.status().lifecycleGeneration
        );
      }
      const committedActor = node.restoreActorAuthority!(
        actor.context.actorId,
        actorType,
        actorRef.generation,
        state.locationGeneration ?? actorRef.generation,
        String(target.spotId),
        spotGeneration,
        membershipEpoch
      );
      state.setNativeActorRef(committedActor);
      if (target.spotKind === ZLinkSpotKind.Entry) state.clearJoinedSpot();
      else state.setJoinedSpot(target.spotId, spot, membershipEpoch, spotGeneration);
      state.setRemoteActorPacketTarget(
        target.spotKind === ZLinkSpotKind.Entry ? undefined : target
      );
    };
    const leaveSource = async (): Promise<void> => {
      void this.options.localSourceLeave?.(actor, previousSpotId, signal).catch((error) => {
        this.options.reportSourceLeaveError(error);
      });
    };
    let response: ZLinkSpotActorJoinResult;
    if (target.spotKind === ZLinkSpotKind.Entry) {
      if (this.options.localEntryJoin === undefined)
        throw new Error('Local Entry Actor Join runtime is not configured.');
      await commit();
      try {
        await this.options.localEntryJoin(actor, signal);
      } finally {
        await leaveSource();
      }
      response = { accepted: true };
    } else {
      if (this.options.localSpotJoin === undefined)
        throw new Error('Local Spot Actor Join runtime is not configured.');
      response = await this.options.localSpotJoin(
        target.spotId,
        actor,
        request,
        commit,
        signal,
        leaveSource,
        frameworkPayloadContentType(request)
      );
    }
    if (response.accepted)
      await this.options.postCommitBinder?.bind(
        toFrameworkActorRef(state.nativeActorRef!, meshName)
      );
    return {
      accepted: response.accepted,
      actor: toFrameworkActorRef(state.nativeActorRef ?? actorRef, meshName),
      reply: response.reply as Message | undefined
    };
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
    return submitJoinWhenConnected(() => completions.submit(submit, signal), timeoutMs, signal);
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
    const completionOperationKey =
      completionOperationId === undefined ? undefined : operationIdentityKey(completionOperationId);
    if (completionOperationKey === relocationId) {
      throw new Error('Actor Join OperationId must be distinct from RelocationId.');
    }
    const actorAuthorityFence = remoteJoinAuthorityFence(node, state);
    // Command 28 is usable only when both the source Actor authority fence
    // and the backend's canonical entry point are present.  Until then keep
    // the existing internal admission route (wire protocol §10); fabricating
    // an Actor fence from the destination Spot would let a stale source pass
    // receiver-side Authority-row equality.
    const canonicalAdmission =
      actorAuthorityFence === undefined || !supportsCanonicalActorJoin(node, entrySpot)
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
        const legacyAdmission =
          canonicalAdmission === undefined
            ? legacyRemoteActorJoinPayload(actor, state, actorRef, target, request, relocationId)
            : undefined;
        const operationId =
          canonicalAdmission === undefined
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
        ? wireReplyFailureException(admission.terminalResult, admission.failureErrno, message)
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
      throw wireReplyFailureException(
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
                      replyContentType: control.replyContentType ?? ZlinkStreamContentType.Raw
                    }),
                reply:
                  completionOperationId === undefined
                    ? Buffer.alloc(0)
                    : Buffer.from(admission.parts[0]?.data() ?? []),
                actorNodeGeneration: actorAuthorityFence!.nodeGeneration,
                expectedOwnerLeaseGeneration: actorAuthorityFence!.ownerLeaseGeneration,
                targetNodeGeneration: target.targetNodeGeneration!,
                targetSpotGeneration: control.location.spotGeneration,
                targetAuthorityOwnerGeneration: actorAuthorityFence!.authorityOwnerGeneration + 1n,
                targetSpotAuthorityOwnerGeneration: target.authorityOwnerGeneration ?? 1n
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
):
  | {
      readonly nodeGeneration: bigint;
      readonly authorityOwnerGeneration: bigint;
      readonly ownerLeaseGeneration: bigint;
    }
  | undefined {
  if (state.locationGeneration === undefined || state.ownerLeaseGeneration === undefined) {
    return undefined;
  }
  return {
    nodeGeneration: node.status().lifecycleGeneration,
    authorityOwnerGeneration: state.locationGeneration,
    ownerLeaseGeneration: state.ownerLeaseGeneration
  };
}

function supportsCanonicalActorJoin(node: ZLinkBackendMeshNode, entrySpot: boolean): boolean {
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
  const payload = Buffer.from(
    JSON.stringify({
      packetName: ZLINK_REMOTE_ACTOR_JOIN_PACKET,
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
    })
  );
  return {
    packetName: ZLINK_FRAMEWORK_ACTOR_JOIN_PACKET_NAME,
    contentType: ZlinkStreamContentType.Json,
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
  return (
    `${hex.slice(0, 8)}-${hex.slice(8, 12)}-${hex.slice(12, 16)}` +
    `-${hex.slice(16, 20)}-${hex.slice(20)}`
  );
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
  const stateMeshName = (state as unknown as { readonly meshName?: string } | undefined)?.meshName;
  return context?.meshName ?? stateMeshName ?? fallback;
}

async function submitJoinWhenConnected<T>(
  submit: () => T,
  timeoutMs: number | undefined,
  signal: AbortSignal | undefined
): Promise<T> {
  const deadline =
    performance.now() +
    Math.min(timeoutMs ?? LOCAL_JOIN_RESOLUTION_TIMEOUT_MS, LOCAL_JOIN_RESOLUTION_TIMEOUT_MS);
  for (;;) {
    throwIfAborted(signal);
    try {
      return submit();
    } catch (error) {
      if (!isBackendNotConnectedError(error) || performance.now() >= deadline) {
        throw error;
      }
      await new Promise<void>((resolve) => setTimeout(resolve, LOCAL_JOIN_POLL_INTERVAL_MS));
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
