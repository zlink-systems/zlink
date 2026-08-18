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
import { ZLinkSpotKind } from '../../contracts';
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
import type { ZLinkRemoteBoundSessionTarget } from './actor-runtime-state';
import type { ZLinkPostCommitActorBinder } from './post-commit-actor-binder';
import type { ZLinkPostCommitActorLocation } from './post-commit-actor-location';
import { toBackendRoutingId as toBackendRoutingId } from '../routing-id';
import { routingIdsEqual } from '../routing-id';
import { operationIdentityKey } from '../foundation/operation-identity';
import {
  buildRemoteActorJoinRequestPayload,
  REMOTE_ACTOR_JOIN_ABORT,
  REMOTE_ACTOR_JOIN_ADMISSION,
} from './actor-remote-wire';
import { encodeFrameworkActorJoinPayload } from '../messaging/actor-join-payload-codec';
import type { ZLinkActorJoinRelocation } from './actor-join-relocation';

export interface ZLinkLocalNativeActorJoinOptions {
  readonly postCommitLocation?: ZLinkPostCommitActorLocation;
  readonly postCommitBinder?: ZLinkPostCommitActorBinder;
  readonly completionTableProvider: () => ZLinkMeshCompletionTable | undefined;
  readonly actorJoinRelocation?: ZLinkActorJoinRelocation;
  readonly messageSerializers?: ReadonlyMap<string, ZLinkMessageSerializer>;
  readonly entrySpotIdProvider?: (meshName: string | undefined) => string | undefined;
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
        encodeFrameworkActorJoinPayload(request),
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
        encodeFrameworkActorJoinPayload(request),
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
    const relocationId = randomUUID();
    const completionOperationKey = completionOperationId === undefined
      ? undefined
      : operationIdentityKey(completionOperationId);
    if (completionOperationKey === relocationId) {
      throw new Error('Actor Join OperationId must be distinct from RelocationId.');
    }
    const admissionPayload = Buffer.from(JSON.stringify(buildRemoteActorJoinRequestPayload({
      actorId: actor.context.actorId,
      actorType,
      actorRef,
      expectedMembershipEpoch: state.spotMembershipEpoch,
      actorEntryNodeRid: state.entryNodeRid ?? actorRef.nodeRid as unknown as RoutingId,
      actorCreateRequest: state.createRequestPayload,
      request,
      targetSpotId: target.spotId,
      routerChannelId: target.routerChannelId,
      sourceSpotId: state.spotId,
      boundSessionTarget: enrichBoundSessionTransferTarget(state),
      phase: REMOTE_ACTOR_JOIN_ADMISSION,
      transferId: relocationId,
      completionOperationId
    })));
    const admission = await this.waitForJoinCompletion(
      () => entrySpot
        ? node.joinActorEntrySpot(
            actorRef,
            toBackendRoutingId(target.targetNodeRid),
            admissionPayload,
            timeoutMs
          )
        : node.joinActorSpot(
            actorRef,
            toBackendRoutingId(target.targetNodeRid),
            toBackendRoutingId(target.spotId),
            target.targetSpotGeneration!,
            admissionPayload,
            timeoutMs
          ),
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
        ? wireReplyFailureException(
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
      throw wireReplyFailureException(
        admission.terminalResult,
        admission.failureErrno,
        `Actor admission failed for '${actor.context.actorId}' with result '${admission.terminalResult}' and errno '${admission.failureErrno}'.`
      );
    }
    disposeParts(admission.parts.slice(1));
    const relocation = this.options.actorJoinRelocation;
    if (relocation === undefined) {
      admission.parts[0]?.close();
      await this.abortRemoteAdmission(
        node,
        completions,
        actorRef,
        target,
        actor,
        state,
        request,
        relocationId,
        timeoutMs,
        signal,
        entrySpot
      );
      throw createInternalFrameworkException(
        ZLinkFrameworkInternalErrorKind.ActorRouteNotFound,
        `Actor '${actor.context.actorId}' canonical remote Join relocation is not configured.`
      );
    }
    let relocated;
    try {
      relocated = await relocation.relocateActorJoin({
        meshName: runtimeActorMeshName(actor, state, target.routerChannelId),
        actor,
        state,
        target,
        relocationId,
        completionOperationId: completionOperationKey,
        advertisedReceiveChunkLimitBytes: control.receiveChunkLimitBytes,
        signal
      });
    } catch (error) {
      admission.parts[0]?.close();
      await this.abortRemoteAdmission(
        node,
        completions,
        actorRef,
        target,
        actor,
        state,
        request,
        relocationId,
        timeoutMs,
        signal,
        entrySpot
      );
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

  private async abortRemoteAdmission(
    node: ZLinkBackendMeshNode,
    completions: ZLinkMeshCompletionTable,
    actorRef: ZLinkBackendActorRef,
    target: ZLinkSpotRouteTarget,
    actor: ZLinkActor,
    state: ZLinkActorRuntimeState,
    request: Message,
    transferId: string,
    timeoutMs: number | undefined,
    signal: AbortSignal | undefined,
    entrySpot = false
  ): Promise<void> {
    try {
      const actorType = state.actorType;
      if (actorType === undefined || (!entrySpot && target.targetSpotGeneration === undefined)) return;
      const payload = Buffer.from(JSON.stringify(buildRemoteActorJoinRequestPayload({
        actorId: actor.context.actorId,
        actorType,
        actorRef,
        expectedMembershipEpoch: state.spotMembershipEpoch,
        actorEntryNodeRid: state.entryNodeRid ?? actorRef.nodeRid as unknown as RoutingId,
        actorCreateRequest: state.createRequestPayload,
        request,
        targetSpotId: target.spotId,
        routerChannelId: target.routerChannelId,
        sourceSpotId: state.spotId ?? requireEntrySpotId(this.options, state.meshName),
        boundSessionTarget: enrichBoundSessionTransferTarget(state),
        phase: REMOTE_ACTOR_JOIN_ABORT,
        transferId
      })));
      const completion = await submitJoinWhenConnected(
        () => completions.submit(() => entrySpot
          ? node.joinActorEntrySpot(
            actorRef,
            toBackendRoutingId(target.targetNodeRid),
            payload,
            timeoutMs
          )
          : node.joinActorSpot(
            actorRef,
            toBackendRoutingId(target.targetNodeRid),
            toBackendRoutingId(target.spotId),
            target.targetSpotGeneration!,
            payload,
            timeoutMs
          ), signal),
        timeoutMs,
        signal
      );
      closeMeshCompletion(completion);
    } catch {
      // Admission cleanup is best effort. The target registry also expires
      // abandoned admissions so a disconnected source cannot retain state.
    }
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

function enrichBoundSessionTransferTarget(state: ZLinkActorRuntimeState): ZLinkRemoteBoundSessionTarget | undefined {
  const target = state.remoteBoundSessionTarget ?? state.boundSessionTransferTarget;
  return target === undefined
    ? undefined
    : {
        ...target,
        previousAuthorityOwnerGeneration:
          target.previousAuthorityOwnerGeneration ?? state.locationGeneration,
        previousOwnerLeaseGeneration:
          target.previousOwnerLeaseGeneration ?? state.ownerLeaseGeneration
      };
}

function requireEntrySpotId(
  options: ZLinkLocalNativeActorJoinOptions,
  meshName: string | undefined
): string {
  const spotId = options.entrySpotIdProvider?.(meshName);
  if (spotId === undefined) {
    throw createInternalFrameworkException(
      ZLinkFrameworkInternalErrorKind.ActorRouteNotFound,
      'Actor source Entry Spot identity is not available.'
    );
  }
  return spotId;
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
