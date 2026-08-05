import { ZLinkFrameworkInternalErrorKind, createInternalFrameworkException  } from '../framework-errors-internal';
import { randomUUID } from 'node:crypto';
import { RequestResult } from '../backend/runtime-values';
import type {
  ActorRef,
  RoutingId,
  ZLinkActor,
  ZLinkActorJoinOperationId,
  ZLinkMessageSerializer,
} from '../../contracts';
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
import type {
  ZLinkActorSourceTransfer,
  ZLinkPreparedActorSource
} from './actor-source-transfer';
import {
  buildRemoteActorJoinRequestPayload,
  REMOTE_ACTOR_JOIN_ABORT,
  REMOTE_ACTOR_JOIN_ADMISSION,
  REMOTE_ACTOR_JOIN_COMMIT,
  ZLINK_REMOTE_ACTOR_SOURCE_LEAVE_TERMINAL
} from './actor-remote-wire';

export interface ZLinkLocalNativeActorJoinOptions {
  readonly postCommitLocation?: ZLinkPostCommitActorLocation;
  readonly postCommitBinder?: ZLinkPostCommitActorBinder;
  readonly completionTableProvider: () => ZLinkMeshCompletionTable | undefined;
  readonly sourceTransfer?: ZLinkActorSourceTransfer;
  readonly messageSerializers?: ReadonlyMap<string, ZLinkMessageSerializer>;
  readonly postCommitErrorReporter?: (error: unknown) => void;
  readonly entrySpotIdProvider?: (meshName: string | undefined) => string | undefined;
  readonly remoteActivationWaiter?: (
    actorId: string,
    targetNodeRid: RoutingId,
    timeoutMs: number | undefined,
    signal?: AbortSignal
  ) => Promise<ActorRef | undefined>;
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
    let prepared: ZLinkPreparedActorSource | undefined;
    let transferId: string | undefined;
    let admissionReply: Message | undefined;
    let abortAdmission: (() => Promise<void>) | undefined;
    const commitState = { attempted: false };
    let requestPayload = Buffer.from(request.data());
    const actorType = state.actorType;
    if (remote && (actorType === undefined || this.options.sourceTransfer === undefined)) {
      throw createInternalFrameworkException(
        ZLinkFrameworkInternalErrorKind.ActorRouteNotFound,
        `Actor '${actor.context.actorId}' remote transfer state is not configured.`
      );
    }
    if (remote) {
      transferId = randomUUID();
      try {
        const admissionPayload = Buffer.from(JSON.stringify(buildRemoteActorJoinRequestPayload({
          actorId: actor.context.actorId,
          actorType: actorType!,
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
          transferId,
          completionOperationId
        })));
        const admissionCompletion = await this.waitForJoinCompletion(
          () => node.joinActorSpot(
            actorRef,
            toBackendRoutingId(target.targetNodeRid),
            toBackendRoutingId(target.spotId),
            target.targetSpotGeneration,
            admissionPayload,
            timeoutMs
          ),
          completions,
          timeoutMs,
          signal
        );
        const admissionControl = admissionCompletion.kindData;
        if (
          admissionControl?.kind !== 'actorJoinCompletion'
          || admissionControl.actor === null
        ) {
          closeMeshCompletion(admissionCompletion);
          throw createInternalFrameworkException(
            ZLinkFrameworkInternalErrorKind.ActorRouteNotFound,
            `Actor admission failed for '${actor.context.actorId}' with result '${admissionCompletion.terminalResult}' and errno '${admissionCompletion.failureErrno}'.`
          );
        }
        if (admissionControl.joinResult !== 0) {
          try {
            return {
              accepted: false,
              actor: toFrameworkActorRef(admissionControl.actor as never, actorMeshName),
              reply: admissionCompletion.parts[0]
            };
          } finally {
            disposeParts(admissionCompletion.parts.slice(1));
          }
        }
        if (admissionCompletion.terminalResult !== 0 || admissionCompletion.failureErrno !== 0) {
          closeMeshCompletion(admissionCompletion);
          throw createInternalFrameworkException(
            ZLinkFrameworkInternalErrorKind.ActorRouteNotFound,
            `Actor admission failed for '${actor.context.actorId}' with result '${admissionCompletion.terminalResult}' and errno '${admissionCompletion.failureErrno}'.`
          );
        }
        admissionReply = admissionCompletion.parts[0];
        disposeParts(admissionCompletion.parts.slice(1));
        abortAdmission = () => this.abortRemoteAdmission(
          node,
          completions,
          actorRef,
          target,
          actor,
          state,
          request,
          requireTransferId(transferId),
          timeoutMs,
          signal
        );

        prepared = await this.options.sourceTransfer!.prepareSource(
          actor,
          state,
          signal,
          'core',
          completionOperationId === undefined
            ? undefined
            : deferredOperationKey(completionOperationId)
        );
        await prepared.reserveTarget(target, signal);
        requestPayload = Buffer.from(JSON.stringify(buildRemoteActorJoinRequestPayload({
          actorId: actor.context.actorId,
          actorType: actorType!,
          actorRef,
          expectedMembershipEpoch: state.spotMembershipEpoch,
          actorEntryNodeRid: state.entryNodeRid ?? actorRef.nodeRid as unknown as RoutingId,
          actorCreateRequest: state.createRequestPayload,
          request,
          targetSpotId: target.spotId,
          routerChannelId: target.routerChannelId,
          sourceSpotId: state.spotId ?? requireEntrySpotId(this.options, state.meshName),
          boundSessionTarget: enrichBoundSessionTransferTarget(state),
          phase: REMOTE_ACTOR_JOIN_COMMIT,
          transferId,
          transferAdapterKey: prepared.adapterKey,
          transferState: prepared.stateReference === undefined
            ? Buffer.from(prepared.state.toEncodedPayload().data())
            : undefined,
          transferStateReference: prepared.stateReference,
          transferStateChecksumCrc32c: prepared.stateChecksumCrc32c,
          handoffBacklog: prepared.handoffBacklog,
          completionOperationId
        })));
      } catch (error) {
        await this.abortRemoteAdmission(
          node,
          completions,
          actorRef,
          target,
          actor,
          state,
          request,
          requireTransferId(transferId),
          timeoutMs,
          signal
        );
        await prepared?.rollback();
        admissionReply?.close();
        throw error;
      }
    }
    let completion: ZLinkMeshCompletion;
    try {
      completion = await this.waitForJoinCompletion(
        () => {
          const operationId = node.joinActorSpot(
            actorRef,
            toBackendRoutingId(target.targetNodeRid),
            toBackendRoutingId(target.spotId),
            target.targetSpotGeneration,
            requestPayload,
            timeoutMs
          );
          commitState.attempted = true;
          return operationId;
        },
        completions,
        timeoutMs,
        signal
      );
    } catch (error) {
      if (!commitState.attempted) {
        await abortAdmission?.();
      }
      await prepared?.rollback();
      admissionReply?.close();
      throw error;
    }
    const control = completion.kindData;
    if (
      control?.kind !== 'actorJoinCompletion' ||
      control.actor === null
    ) {
      await prepared?.rollback();
      admissionReply?.close();
      closeMeshCompletion(completion);
      throw createInternalFrameworkException(
        ZLinkFrameworkInternalErrorKind.ActorRouteNotFound,
        `Actor join failed for '${actor.context.actorId}' with result '${completion.terminalResult}' and errno '${completion.failureErrno}'.`
      );
    }
    if (control.joinResult !== 0) {
      try {
        if (abortAdmission !== undefined) {
          await abortAdmission();
        }
        await prepared?.rollback();
        return {
          accepted: false,
          actor: toFrameworkActorRef(control.actor as never, actorMeshName),
          reply: completion.parts[0]
        };
      } finally {
        disposeParts(completion.parts.slice(1));
        admissionReply?.close();
      }
    }
    if (completion.terminalResult !== 0 || completion.failureErrno !== 0) {
      await prepared?.rollback();
      admissionReply?.close();
      closeMeshCompletion(completion);
      throw createInternalFrameworkException(
        ZLinkFrameworkInternalErrorKind.ActorRouteNotFound,
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
    state.setRemoteActorPacketTarget(undefined);
    if (remote) {
      const nativeTargetActorRef = toFrameworkActorRef(
        control.actor as never,
        actorMeshName
      );
      try {
        // The Core leave callback is a late lifecycle notification. Waiting
        // for it before changing Actor authority deadlocks the remote Join:
        // the target waits for this source terminal before publishing its
        // ownership, while Core emits the source leave after the source
        // authority commit. The source handoff remains held by the transfer
        // coordinator; only the authority transition moves ahead of the
        // callback. Source shell cleanup is deferred until that callback.
        await prepared?.commitAuthority(target, nativeTargetActorRef, signal);
      } catch (error) {
        await this.publishSourceLeaveTerminal(
          node,
          completions,
          target.targetNodeRid,
          actor.context.actorId,
          requireTransferId(transferId),
          false,
          timeoutMs,
          signal
        );
        prepared?.commit(
          target,
          toFrameworkActorRef(control.actor as never, actorMeshName),
          []
        );
        throw error;
      }
      await this.publishSourceLeaveTerminal(
        node,
        completions,
        target.targetNodeRid,
        actor.context.actorId,
        requireTransferId(transferId),
        true,
        timeoutMs,
        signal
      );
      prepared?.commit(target, nativeTargetActorRef, []);
      try {
        await this.options.remoteActivationWaiter?.(
          actor.context.actorId,
          target.targetNodeRid,
          timeoutMs,
          signal
        );
      } catch (error) {
        this.options.postCommitErrorReporter?.(error);
      }
    } else {
      prepared?.commit(
        target,
        toFrameworkActorRef(control.actor as never, actorMeshName),
        []
      );
    }
    if (!remote && state.actorType !== undefined) {
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
    const finalReply = admissionReply ?? completion.parts[0];
    try {
      return {
        accepted: true,
        actor: toFrameworkActorRef(
          control.actor as never,
          actorMeshName
        ),
        reply: finalReply
      };
    } finally {
      if (admissionReply === undefined) {
        disposeParts(completion.parts.slice(1));
      } else {
        disposeParts(completion.parts);
      }
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
    let prepared: ZLinkPreparedActorSource | undefined;
    let transferId: string | undefined;
    let admissionReply: Message | undefined;
    let abortAdmission: (() => Promise<void>) | undefined;
    const commitState = { attempted: false };
    let requestPayload = Buffer.from(request.data());
    if (remote) {
      const actorType = state.actorType;
      if (actorType === undefined || this.options.sourceTransfer === undefined) {
        throw createInternalFrameworkException(
          ZLinkFrameworkInternalErrorKind.ActorRouteNotFound,
          `Actor '${actor.context.actorId}' remote Entry Spot transfer state is not configured.`
        );
      }
      transferId = randomUUID();
      try {
        const admissionPayload = Buffer.from(JSON.stringify(buildRemoteActorJoinRequestPayload({
          actorId: actor.context.actorId,
          actorType,
          actorRef,
          expectedMembershipEpoch: state.spotMembershipEpoch,
          actorEntryNodeRid: state.entryNodeRid ?? actorRef.nodeRid as unknown as RoutingId,
          actorCreateRequest: state.createRequestPayload,
          request,
          targetSpotId: spotRouteTarget!.spotId,
          routerChannelId: spotRouteTarget!.routerChannelId,
          sourceSpotId: state.spotId,
          boundSessionTarget: enrichBoundSessionTransferTarget(state),
          phase: REMOTE_ACTOR_JOIN_ADMISSION,
          transferId,
          completionOperationId
        })));
        const admissionCompletion = await this.waitForJoinCompletion(
          () => node.joinActorEntrySpot(
            actorRef,
            toBackendRoutingId(targetNodeRid),
            admissionPayload,
            timeoutMs
          ),
          completions,
          timeoutMs,
          signal
        );
        const admissionControl = admissionCompletion.kindData;
        if (
          admissionControl?.kind !== 'actorJoinCompletion'
          || admissionControl.actor === null
        ) {
          closeMeshCompletion(admissionCompletion);
          throw createInternalFrameworkException(
            ZLinkFrameworkInternalErrorKind.ActorRouteNotFound,
            `Actor Entry Spot admission failed for '${actor.context.actorId}' with result '${admissionCompletion.terminalResult}' and errno '${admissionCompletion.failureErrno}'.`
          );
        }
        if (admissionControl.joinResult !== 0) {
          try {
            return {
              accepted: false,
              actor: toFrameworkActorRef(admissionControl.actor as never, actorMeshName),
              reply: admissionCompletion.parts[0]
            };
          } finally {
            disposeParts(admissionCompletion.parts.slice(1));
          }
        }
        if (admissionCompletion.terminalResult !== 0 || admissionCompletion.failureErrno !== 0) {
          closeMeshCompletion(admissionCompletion);
          throw createInternalFrameworkException(
            ZLinkFrameworkInternalErrorKind.ActorRouteNotFound,
            `Actor Entry Spot admission failed for '${actor.context.actorId}' with result '${admissionCompletion.terminalResult}' and errno '${admissionCompletion.failureErrno}'.`
          );
        }
        admissionReply = admissionCompletion.parts[0];
        disposeParts(admissionCompletion.parts.slice(1));
        abortAdmission = () => this.abortRemoteAdmission(
          node,
          completions,
          actorRef,
          spotRouteTarget!,
          actor,
          state,
          request,
          requireTransferId(transferId),
          timeoutMs,
          signal,
          true
        );
        prepared = await this.options.sourceTransfer.prepareSource(
          actor,
          state,
          signal,
          'core',
          completionOperationId === undefined
            ? undefined
            : deferredOperationKey(completionOperationId)
        );
        await prepared.reserveTarget(spotRouteTarget, signal);
        requestPayload = Buffer.from(JSON.stringify(buildRemoteActorJoinRequestPayload({
          actorId: actor.context.actorId,
          actorType,
          actorRef,
          expectedMembershipEpoch: state.spotMembershipEpoch,
          actorEntryNodeRid: state.entryNodeRid ?? actorRef.nodeRid as unknown as RoutingId,
          actorCreateRequest: state.createRequestPayload,
          request,
          targetSpotId: spotRouteTarget.spotId,
          routerChannelId: spotRouteTarget.routerChannelId,
          sourceSpotId: state.spotId ?? requireEntrySpotId(this.options, state.meshName),
          boundSessionTarget: enrichBoundSessionTransferTarget(state),
          phase: REMOTE_ACTOR_JOIN_COMMIT,
          transferId,
          transferAdapterKey: prepared.adapterKey,
          transferState: prepared.stateReference === undefined
            ? Buffer.from(prepared.state.toEncodedPayload().data())
            : undefined,
          transferStateReference: prepared.stateReference,
          transferStateChecksumCrc32c: prepared.stateChecksumCrc32c,
          handoffBacklog: prepared.handoffBacklog,
          completionOperationId
        })));
      } catch (error) {
        await this.abortRemoteAdmission(
          node,
          completions,
          actorRef,
          spotRouteTarget!,
          actor,
          state,
          request,
          requireTransferId(transferId),
          timeoutMs,
          signal,
          true
        );
        await prepared?.rollback();
        admissionReply?.close();
        throw error;
      }
    }

    let completion: ZLinkMeshCompletion;
    try {
      completion = await this.waitForJoinCompletion(
        () => {
          const operationId = node.joinActorEntrySpot(
            actorRef,
            toBackendRoutingId(targetNodeRid),
            requestPayload,
            timeoutMs
          );
          commitState.attempted = true;
          return operationId;
        },
        completions,
        timeoutMs,
        signal
      );
    } catch (error) {
      if (!commitState.attempted) {
        await abortAdmission?.();
      }
      await prepared?.rollback();
      admissionReply?.close();
      throw error;
    }
    const control = completion.kindData;
    if (
      control?.kind !== 'actorJoinCompletion' ||
      control.actor === null
    ) {
      await prepared?.rollback();
      admissionReply?.close();
      closeMeshCompletion(completion);
      throw createInternalFrameworkException(
        ZLinkFrameworkInternalErrorKind.ActorRouteNotFound,
        `Actor entry SPOT join failed for '${actor.context.actorId}' with result '${completion.terminalResult}' and errno '${completion.failureErrno}'.`
      );
    }
    if (control.joinResult !== 0) {
      try {
        if (abortAdmission !== undefined) {
          await abortAdmission();
        }
        await prepared?.rollback();
        return {
          accepted: false,
          actor: toFrameworkActorRef(control.actor as never, actorMeshName),
          reply: completion.parts[0]
        };
      } finally {
        disposeParts(completion.parts.slice(1));
        admissionReply?.close();
      }
    }
    if (completion.terminalResult !== 0 || completion.failureErrno !== 0) {
      await prepared?.rollback();
      admissionReply?.close();
      closeMeshCompletion(completion);
      throw createInternalFrameworkException(
        ZLinkFrameworkInternalErrorKind.ActorRouteNotFound,
        `Actor entry SPOT join failed for '${actor.context.actorId}' with result '${completion.terminalResult}' and errno '${completion.failureErrno}'.`
      );
    }

    state.setNativeActorRef(control.actor as never);
    state.clearJoinedSpot();
    state.setRemoteActorPacketTarget(undefined);
    if (remote) {
      const nativeTargetActorRef = toFrameworkActorRef(
        control.actor as never,
        actorMeshName
      );
      try {
        await prepared?.commitAuthority(spotRouteTarget!, nativeTargetActorRef, signal);
      } catch (error) {
        await this.publishSourceLeaveTerminal(
          node,
          completions,
          spotRouteTarget!.targetNodeRid,
          actor.context.actorId,
          requireTransferId(transferId),
          false,
          timeoutMs,
          signal
        );
        prepared?.commit(spotRouteTarget!, nativeTargetActorRef, []);
        admissionReply?.close();
        throw error;
      }
      await this.publishSourceLeaveTerminal(
        node,
        completions,
        spotRouteTarget!.targetNodeRid,
        actor.context.actorId,
        requireTransferId(transferId),
        true,
        timeoutMs,
        signal
      );
      prepared?.commit(spotRouteTarget!, nativeTargetActorRef, []);
      try {
        await this.options.remoteActivationWaiter?.(
          actor.context.actorId,
          spotRouteTarget!.targetNodeRid,
          timeoutMs,
          signal
        );
      } catch (error) {
        this.options.postCommitErrorReporter?.(error);
      }
    } else if (state.actorType !== undefined) {
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
      admissionReply?.close();
    }
  }

  private async waitForJoinCompletion(
    submit: () => { readonly high: bigint; readonly low: bigint },
    completions: ZLinkMeshCompletionTable,
    timeoutMs: number | undefined,
    signal: AbortSignal | undefined
  ): Promise<ZLinkMeshCompletion> {
    const operationId = await submitJoinWhenConnected(submit, timeoutMs, signal);
    // The operation ID identifies one Core request. Once it is submitted, a
    // NotConnected completion is terminal for that request; resubmitting it
    // could execute the target lifecycle twice after a delayed reply.
    return completions.wait(operationId, signal);
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
      const operationId = await submitJoinWhenConnected(
        () => entrySpot
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
            ),
        timeoutMs,
        signal
      );
      const completion = await completions.wait(operationId, signal);
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

  private async publishSourceLeaveTerminal(
    node: ZLinkBackendMeshNode,
    completions: ZLinkMeshCompletionTable,
    targetNodeRid: RoutingId,
    actorId: string,
    transferId: string,
    succeeded: boolean,
    timeoutMs: number | undefined,
    signal: AbortSignal | undefined
  ): Promise<void> {
    const deadline = Date.now() + (timeoutMs ?? 30_000);
    const payload = Buffer.from(JSON.stringify({
      packetName: ZLINK_REMOTE_ACTOR_SOURCE_LEAVE_TERMINAL,
      transferId,
      actorId,
      succeeded
    }));
    for (;;) {
      throwIfAborted(signal);
      const remainingMs = Math.max(1, deadline - Date.now());
      let completion;
      try {
        const operationId = node.requestToNode(
          toBackendRoutingId(targetNodeRid),
          payload,
          { timeoutMs: remainingMs }
        );
        completion = await completions.wait(operationId, signal);
      } catch (error) {
        if (!isRetryableTerminalRouteFailure(error) || Date.now() >= deadline) {
          throw error;
        }
        await new Promise<void>((resolve) => setTimeout(resolve, 10));
        continue;
      }
      const retry = completion.terminalResult === RequestResult.NotConnected
        && Date.now() < deadline;
      try {
        if (!retry && (completion.terminalResult !== 0 || completion.failureErrno !== 0)) {
          throw createInternalFrameworkException(
            ZLinkFrameworkInternalErrorKind.ActorRouteNotFound,
            `Actor '${actorId}' source leave terminal result was not acknowledged.`
          );
        }
      } finally {
        closeMeshCompletion(completion);
      }
      if (!retry) return;
      await new Promise<void>((resolve) => setTimeout(resolve, 10));
    }
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

function isRetryableTerminalRouteFailure(error: unknown): boolean {
  return error instanceof Error && (
    error.message.includes('target route is not connected')
    || error.message.includes('Transport endpoint is not connected')
  );
}

function requireTransferId(transferId: string | undefined): string {
  if (transferId === undefined) {
    throw new Error('Remote actor join has no private transfer id.');
  }
  return transferId;
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
      if (
        !(error instanceof Error) ||
        !error.message.includes('Transport endpoint is not connected') ||
        Date.now() >= deadline
      ) {
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

function deferredOperationKey(
  operationId: ZLinkActorJoinOperationId
): string {
  return `${operationId.high.toString(16)}:${operationId.low.toString(16)}`;
}
