import { randomBytes } from 'node:crypto';
import { SubmitResult } from '../backend/runtime-values';
import type {
  RoutingId,
  Type,
  ZLinkActor,
  ZLinkActorRelocationAdapter,
  ZLinkMeshNodeDescriptor,
  ZLinkRelocationStore,
  ZLinkSpot,
  ZLinkSpotRelocationAdapter,
  ZLinkInstanceSpot
} from '../../contracts';
import type {
  ZLinkAuthorityKey,
  ZLinkAuthoritySnapshot,
  ZLinkCapacityVector,
  ZLinkLocationOwnerToken
} from '../../contracts/Locations';
import {
  ZLinkFrameworkRuntimeState,
  ZLinkObjectRole,
  ZLinkSpotRelocationReadinessMode,
  ZLinkSpotRelocationReadyOutcome,
  ZLinkSpotKind,
  ZLinkUserSpotExecutionMode
} from '../../contracts';
import { zlinkRuntimeDefaultLocationOptions } from '../../contracts/Locations/Options';
import {
  ZLinkFrameworkInternalErrorKind,
  createInternalFrameworkException
} from '../framework-errors-internal';
import type { ZLinkDomainLocationStore as ZLinkLocationStore } from '../locations/domain-store-contract';
import type { ZLinkTrackedInstanceAuthority } from '../locations/spot-location-claims';
import {
  putNewRelocationBlob,
  relocationBlobReference
} from '../locations/relocation-blob';
import type { ZLinkProviderResolver } from '../../contracts/Common/ZLinkProviderResolver';
import type {
  ZLinkRuntimeEventPublisher,
  ZLinkRuntimeMetrics
} from '../diagnostics';
import type { ZLinkFrameworkRegistration } from '../configuration';
import type {
  ZLinkBackendMeshNode,
  ZLinkMeshCompletionTable
} from '../backend';
import { ReceiveKind, type ReceiveRecord } from '../foundation/service-runtime-contracts';
import { routingIdsEqual } from '../routing-id';
import { ServiceWireProtocolError } from '../foundation/service-wire-m6a-codec';
import type { ServiceSpotMessageFollowSeal } from '../foundation/service-stateful-runtime';
import {
  ServiceRelocationAuthorityPayloadCodec,
  crc32c,
  decodeServiceRelocationEnvelope,
  encodeServiceRelocationEnvelope,
  inventoryDigest,
  serviceRelocationAuthorityApplicationPayload,
  type ServiceRelocationEnvelope,
  type ServiceRelocationMembership,
  type ServiceRelocationParticipant,
  type ServiceRelocationPublication,
  type ServiceRelocationQueuedMessage,
  type ServiceRelocationStorePort,
  type ServiceRelocationTimer
} from '../foundation/service-relocation-runtime';
import {
  ServiceRelocationPostCommitError
} from '../foundation/service-relocation-coordinator';
import {
  ServiceMaintenanceRuntime
} from '../foundation/service-maintenance-runtime';
import {
  ServiceRelocationObjectCaptureOwner,
  ServiceRelocationObjectRestoreOwner,
  type ServiceCapturedObjectRelocation,
  type ServiceObjectRelocationStaging,
  type ServiceRelocationCaptureUnit,
  type ServiceRelocationHiddenObject,
  type ServiceRelocationTargetObjectPort
} from '../foundation/service-relocation-object-owner';
import {
  ServiceRelocationAggregateCommitter,
  type ServicePreparedRelocationAggregate,
  type ServiceRelocationAggregatePlan
} from '../foundation/service-relocation-aggregate-committer';
import { createProviderInstance } from '../spots/spot-provider';
import type {
  DefaultZLinkSpotManager,
  ZLinkSpotNodeRuntimeManager
} from '../spots';
import type { ZLinkSpotActivation } from '../spots/spot-activation-state';
import type { DefaultZLinkActorManager } from '../actors';
import {
  toFrameworkActorRef,
  type ZLinkActorRuntimeState,
  type ZLinkRemoteBoundSessionTarget
} from '../actors/actor-runtime-state';
import {
  replayActorHandoffBacklog,
  type ZLinkActorHandoffPacket,
  type ZLinkActorHandoffResult
} from '../actors/actor-handoff';
import { decodeHandoffBacklog } from '../spots/spot-remote-codec';
import {
  decodeActorAuthorityIdentity,
  rewriteActorAuthorityRoute
} from '../actors/actor-authority-publication';
import { rewriteServiceAuthorityRoute } from '../foundation/service-authority-payload-codec';
import {
  committedActorOwnerFence,
  type ZLinkActorTransferRuntime
} from './actor-transfer-runtime';
import { decodeAuthorityKey, encodeAuthorityKey } from '../locations/authority-key-codec';
import {
  decodeServiceRelocationControlRequest,
  decodeServiceRelocationControlResponse,
  encodeServiceRelocationControlRequest,
  encodeServiceRelocationControlResponse,
  type ZLinkServiceRelocationControlRequest,
  type ZLinkServiceRelocationControlResponse
} from './service-relocation-control';
import {
  decodeMaintenanceReplyRelay,
  decodeMaintenanceReplyRelayAck,
  decodeSessionRelocationRoute,
  decodeSessionRelocationSeal,
  decodeSessionRelocationSealed,
  encodeMaintenanceReplyRelay,
  encodeMaintenanceReplyRelayAck,
  encodeSessionRelocationRoute,
  encodeSessionRelocationSeal,
  encodeSessionRelocationSealed,
  encodeServiceWireFrozenActorApplicationRecord,
  M6bServiceWireCommand,
  type ServiceMaintenanceReplyRelay,
  type ServiceMaintenanceReplyRelayAck,
  type ServiceMaintenanceRelocationControl,
  type ServiceMaintenanceRelocationCutover,
  type ServiceMaintenanceRelocationData,
  type ServiceMaintenanceRelocationReady,
  type ServiceMaintenanceRelocationPrepare,
  type ServiceSessionRelocationRoute,
  type ServiceSessionRelocationSeal,
  type ServiceSessionRelocationSealed,
  type ServiceWireOperationId,
  type ServiceWireRequestSourceFence,
  type ServiceWireRelocationTarget,
  type ServiceWireRelocationCoordinatorFence,
  type ServiceWireRelocationObject,
} from '../foundation/service-stateful-wire-codec';
import { BoundedReplayMap } from './bounded-replay-map';
import type { ZLinkActorJoinRelocation } from '../actors/actor-join-relocation';
import {
  decodeRemoteActorSourceLeaveTerminal,
  ZLINK_REMOTE_ACTOR_SOURCE_LEAVE_TERMINAL
} from '../actors/actor-remote-wire';

export class ZLinkRelocationStateIncompatibleError extends Error {
  constructor(message: string) {
    super(message);
    this.name = 'ZLinkRelocationStateIncompatibleError';
  }
}

const RELOCATION_STRIPE_BYTES = 64 * 1024 * 1024;
const RELOCATION_STRIPE_CONCURRENCY = 64;
const RELOCATION_STRIPE_LIMIT = 4096;
const RELOCATION_STRIPE_MANIFEST_KIND = 'zlink-relocation-striped-v1';
const RELOCATION_TARGET_TOMBSTONE_LIMIT = 1024;
const RELOCATION_OPERATION_RETENTION_MS = 5 * 60_000;
const SERVICE_CONTROL_TERMINAL_CAPACITY = 4096;

interface ZLinkHostRelocationOptions {
  readonly registration: ZLinkFrameworkRegistration;
  readonly providerResolver?: ZLinkProviderResolver;
  readonly locationStore: () => ZLinkLocationStore | undefined;
  readonly relocationStore: () => ZLinkRelocationStore | undefined;
  readonly currentOwner: () => ZLinkLocationOwnerToken | undefined;
  readonly liveDescriptors: (
    meshName: string,
    signal?: AbortSignal
  ) => Promise<readonly ZLinkMeshNodeDescriptor[]>;
  readonly localDescriptor?: (meshName: string) => ZLinkMeshNodeDescriptor | undefined;
  readonly meshNode: (meshName: string) => ZLinkBackendMeshNode | undefined;
  readonly completions: (meshName: string) => ZLinkMeshCompletionTable | undefined;
  readonly spotManager: () => DefaultZLinkSpotManager | undefined;
  readonly spotNodeRuntime: () => ZLinkSpotNodeRuntimeManager | undefined;
  readonly actorManager: () => DefaultZLinkActorManager | undefined;
  readonly actorTransfer: ZLinkActorTransferRuntime;
  readonly boundSessionRelocation?: {
    receiveSeal(
      value: ServiceSessionRelocationSeal,
      signal?: AbortSignal
    ): Promise<ServiceSessionRelocationSealed>;
    receiveRoute(value: ServiceSessionRelocationRoute): Promise<void>;
    clear?(): void;
  };
  readonly trackInstanceSpot?: (input: ZLinkTrackedInstanceAuthority) => void;
  readonly reconcileStatefulAuthorityRoutes?: (signal?: AbortSignal) => Promise<void>;
  readonly runtimeEventPublisher?: ZLinkRuntimeEventPublisher;
  readonly metrics?: ZLinkRuntimeMetrics;
}

interface RelocationTargetRequirement {
  readonly objectKind: 'actor' | 'user_spot' | 'instance_spot';
  readonly stableType: string;
  readonly policy: 'disabled' | 'recreate' | 'snapshot';
  readonly count: number;
}

interface LocalHidden extends ServiceRelocationHiddenObject {
  readonly participant: ServiceRelocationParticipant;
  readonly actor?: ZLinkActor;
  readonly activation?: ZLinkSpotActivation;
  initialized: boolean;
  readonly restoredTimers: ServiceRelocationTimer[];
  readonly replayResults: ZLinkActorHandoffResult[];
  readonly replayPackets: ZLinkActorHandoffPacket[];
  targetAuthorityOwnerGeneration?: bigint;
}

interface LocalStage {
  readonly offer: TargetRelocationOffer;
  readonly owner: ServiceRelocationObjectRestoreOwner<LocalHidden>;
  readonly staging: ServiceObjectRelocationStaging<LocalHidden>;
  phase: 'ready' | 'finalizing' | 'committed' | 'open' | 'failed';
  lane: Promise<void>;
  cutoverReceived: boolean;
  fallback?: ReturnType<typeof setTimeout>;
  finalize?: Promise<void>;
}

interface TargetRelocationReservation {
  readonly prepared: ServicePreparedRelocationAggregate;
}

interface TargetRelocationOffer {
  readonly prepare: ServiceMaintenanceRelocationPrepare;
  readonly prepareFingerprint: string;
  readonly authenticatedSourceNodeRid: string;
  readonly envelope: ServiceRelocationEnvelope;
  readonly restoreDeadlineAtMs: number;
  readonly reservation: TargetRelocationReservation;
}

interface TargetPrepareOperation {
  readonly fingerprint: string;
  readonly promise: Promise<ServiceMaintenanceRelocationReady>;
}

interface SourceActorSession {
  readonly state: ZLinkActorRuntimeState;
  readonly actor: ZLinkActor;
  readonly prepared: Awaited<ReturnType<ZLinkActorTransferRuntime['prepareMaintenanceSession']>>;
}

interface SourceActorJoinProfile {
  readonly actor: ZLinkActor;
  readonly state: ZLinkActorRuntimeState;
  readonly sourceActorRef: NonNullable<ZLinkActorRuntimeState['nativeActorRef']>;
  readonly sourceSpotId: RoutingId | undefined;
  readonly completionOperationId?: string;
  readonly ready: Promise<void>;
  readonly resolveReady: () => void;
}

interface PendingRelocationControl {
  readonly targetNodeRid: string;
  readonly request: ZLinkServiceRelocationControlRequest;
  readonly resolve: (response: ZLinkServiceRelocationControlResponse) => void;
  readonly reject: (error: unknown) => void;
  timer?: ReturnType<typeof setTimeout>;
}

interface PendingRelocationReplyRelay {
  readonly ackTargetNodeRid: string;
  readonly identityKey: string;
  readonly request: ServiceMaintenanceReplyRelay;
  readonly expectedRequestSource: ServiceWireRequestSourceFence;
  readonly resolve: (delivery: RelocationTerminalDelivery) => void;
  readonly reject: (error: unknown) => void;
  timer?: ReturnType<typeof setTimeout>;
}

interface PendingSessionRelocation {
  readonly targetNodeRid: string;
  readonly request: ServiceSessionRelocationSeal;
  readonly requestFingerprint: string;
  readonly promise: Promise<ServiceSessionRelocationSealed>;
  readonly resolve: (response: ServiceSessionRelocationSealed) => void;
  readonly reject: (error: unknown) => void;
  retryTimer?: ReturnType<typeof setTimeout>;
  deadlineTimer?: ReturnType<typeof setTimeout>;
}

interface TerminalSessionRelocationControl {
  readonly targetNodeRid: string;
  readonly request: ServiceSessionRelocationSeal;
  readonly requestFingerprint: string;
  readonly responseFingerprint: string;
}

/** Production host bridge from Retire inventory to remote RouteMesh owners. */
export class ZLinkHostServiceRelocationRuntime implements ZLinkActorJoinRelocation {
  private readonly targetStages = new Map<string, LocalStage>();
  private readonly terminalTargets = new BoundedReplayMap<string, string>(
    RELOCATION_TARGET_TOMBSTONE_LIMIT
  );
  private readonly relocationAuthorityKeys = new Map<string, string>();
  private readonly pendingControls = new Map<string, PendingRelocationControl>();
  private readonly targetPrepareOperations = new Map<string, TargetPrepareOperation>();
  private readonly pendingReplyRelays = new Map<string, PendingRelocationReplyRelay>();
  private readonly pendingSessionRelocations = new Map<string, PendingSessionRelocation>();
  private readonly terminalSessionRelocations =
    new BoundedReplayMap<string, TerminalSessionRelocationControl>(
      SERVICE_CONTROL_TERMINAL_CAPACITY
    );
  private readonly sourceRelocationIds = new Set<string>();
  private readonly sourceActorJoinProfiles = new Map<string, SourceActorJoinProfile>();
  private readonly terminalActorJoinSourceLeaves =
    new BoundedReplayMap<string, string>(SERVICE_CONTROL_TERMINAL_CAPACITY);
  private readonly codec = new ServiceRelocationAuthorityPayloadCodec();

  constructor(private readonly options: ZLinkHostRelocationOptions) {}

  async relocateActorJoin(
    input: Parameters<ZLinkActorJoinRelocation['relocateActorJoin']>[0]
  ): ReturnType<ZLinkActorJoinRelocation['relocateActorJoin']> {
    const sourceRef = input.state.nativeActorRef;
    if (sourceRef === undefined) {
      throw new Error(`Actor '${input.state.actorId}' has no source authority identity.`);
    }
    const descriptors = await this.options.liveDescriptors(input.meshName, input.signal);
    const target = descriptors.find(value => routingIdsEqual(value.rid, input.target.targetNodeRid));
    if (target === undefined) {
      throw new Error(
        `Actor '${input.state.actorId}' Join target '${String(input.target.targetNodeRid)}' is not live.`
      );
    }
    if (input.target.targetNodeGeneration !== undefined
      && target.lifecycleGeneration !== input.target.targetNodeGeneration) {
      throw new Error(`Actor '${input.state.actorId}' Join target generation changed after admission.`);
    }
    this.reserveExactRelocationId(input.relocationId);
    let resolveReady!: () => void;
    const ready = new Promise<void>((resolve) => {
      resolveReady = resolve;
    });
    this.sourceActorJoinProfiles.set(input.relocationId, {
      actor: input.actor,
      state: input.state,
      sourceActorRef: sourceRef,
      sourceSpotId: input.state.spotId,
      completionOperationId: input.completionOperationId,
      ready,
      resolveReady
    });
    const membershipEpoch = input.state.spotMembershipEpoch > 0n
      ? input.state.spotMembershipEpoch
      : 1n;
    const spotGeneration = input.target.targetSpotGeneration ?? target.lifecycleGeneration;
    try {
      await this.relocateStandaloneActor(
        input.meshName,
        input.state,
        target,
        target.applicationVersion,
        input.signal,
        {
          spotId: input.target.spotId,
          spotKind: input.target.spotKind === ZLinkSpotKind.Entry
            ? ZLinkSpotKind.Entry
            : ZLinkSpotKind.User,
          spotObjectGeneration: spotGeneration
        },
        input.relocationId,
        true
      );
      resolveReady();
    } catch (error) {
      this.sourceActorJoinProfiles.delete(input.relocationId);
      throw error;
    }
    return {
      actorRef: {
        actorId: sourceRef.actorId,
        generation: sourceRef.generation,
        nodeRid: target.rid
      },
      membershipEpoch,
      spotGeneration
    };
  }

  async dispose(): Promise<void> {
    const errors: unknown[] = [];
    for (const stage of this.targetStages.values()) {
      if (stage.fallback !== undefined) clearTimeout(stage.fallback);
      try {
        if (stage.finalize !== undefined) {
          await stage.finalize;
        } else if (stage.phase === 'ready') {
          await this.abortTargetStage(stage);
        }
      } catch (error) {
        errors.push(error);
      }
    }
    this.targetStages.clear();
    this.terminalTargets.clear();
    const stopped = new Error('Relocation runtime stopped.');
    stopped.name = 'AbortError';
    for (const pending of this.pendingControls.values()) {
      if (pending.timer !== undefined) clearTimeout(pending.timer);
      pending.reject(stopped);
    }
    this.pendingControls.clear();
    this.targetPrepareOperations.clear();
    for (const pending of this.pendingReplyRelays.values()) {
      if (pending.timer !== undefined) clearTimeout(pending.timer);
      pending.reject(stopped);
    }
    this.pendingReplyRelays.clear();
    for (const pending of this.pendingSessionRelocations.values()) {
      if (pending.retryTimer !== undefined) clearTimeout(pending.retryTimer);
      if (pending.deadlineTimer !== undefined) clearTimeout(pending.deadlineTimer);
      pending.reject(stopped);
    }
    this.pendingSessionRelocations.clear();
    this.terminalSessionRelocations.clear();
    this.sourceActorJoinProfiles.clear();
    this.terminalActorJoinSourceLeaves.clear();
    this.options.boundSessionRelocation?.clear?.();
    this.relocationAuthorityKeys.clear();
    if (errors.length === 1) throw errors[0];
    if (errors.length > 1) throw new AggregateError(errors, 'Relocation runtime stop failed.');
  }

  completeActorJoinSourceCleanup(actorId: string): void {
    for (const [relocationId, profile] of this.sourceActorJoinProfiles) {
      if (profile.state.actorId !== actorId) continue;
      this.sourceActorJoinProfiles.delete(relocationId);
      this.terminalActorJoinSourceLeaves.remember(relocationId, actorId);
      void profile.ready.then(async () => {
        await this.options.actorTransfer.completeRelocationSourceLeave(
          profile.actor,
          profile.sourceSpotId
        );
        await this.options.actorManager()?.completeRelocationSource(
          actorId,
          profile.sourceActorRef
        );
      }).catch(error => {
        console.error('[zlink.runtime.relocation.source_cleanup_failed]', error);
      });
    }
  }

  async relocateMesh(
    meshName: string,
    targetApplicationVersion?: bigint,
    signal?: AbortSignal,
    stopStartingSignal?: AbortSignal
  ): Promise<void> {
    const spotManager = this.options.spotManager();
    const actorManager = this.options.actorManager();
    // A stateless host has no relocation work. The corresponding managers are
    // not created for that registration, and the no-op path must still allow
    // the host relocation lifecycle to complete.
    if (spotManager === undefined || actorManager === undefined) return;
    const groupedActorIds = new Set<string>();
    const work: Array<{
      readonly id: string;
      readonly relocate: (relocationSignal: AbortSignal) => Promise<void>;
    }> = [];
    for (const activation of spotManager.relocationActivations(meshName)) {
      const kind = this.spotKind(meshName, activation);
      if (kind === undefined) continue;
      const states = actorManager.snapshotStates().filter(state =>
        state.actor !== undefined && String(state.spotId) === String(activation.spotId));
      for (const state of states) groupedActorIds.add(state.actorId);
      if (
        kind === 'user_spot'
        && activation.executionMode === ZLinkUserSpotExecutionMode.PerActor
      ) {
        let shellRelocation: Promise<void> | undefined;
        const relocateShell = (relocationSignal: AbortSignal): Promise<void> => {
          shellRelocation ??= this.relocatePerActorSpotShell(
            meshName,
            activation,
            undefined,
            targetApplicationVersion,
            relocationSignal
          );
          return shellRelocation;
        };
        work.push({
          id: `spot:${String(activation.spotId)}`,
          relocate: relocateShell
        });
        for (const state of states) {
          work.push({
            id: `actor:${state.actorId}`,
            relocate: async relocationSignal => {
              await relocateShell(relocationSignal);
              await this.relocateStandaloneActor(
                meshName,
                state,
                undefined,
                targetApplicationVersion,
                relocationSignal,
                {
                  spotId: activation.spotId,
                  spotKind: ZLinkSpotKind.User,
                  spotObjectGeneration: (
                    await requireAuthority(
                      this.requireLocationStore(),
                      encodeAuthorityKey('user_spot', String(activation.spotId)),
                      relocationSignal
                    )
                  ).objectGeneration
                }
              );
            }
          });
        }
        continue;
      }
      work.push({
        id: `spot:${String(activation.spotId)}`,
        relocate: relocationSignal =>
          this.relocateSpotAggregate(
            meshName, activation, kind, states, undefined, targetApplicationVersion, relocationSignal)
      });
    }

    for (const state of actorManager.snapshotStates()) {
      if (state.actor === undefined || groupedActorIds.has(state.actorId)) continue;
      if ((state.meshName ?? meshName) !== meshName) continue;
      work.push({
        id: `actor:${state.actorId}`,
        relocate: relocationSignal =>
          this.relocateStandaloneActor(
            meshName, state, undefined, targetApplicationVersion, relocationSignal)
      });
    }
    if (work.length === 0) return;
    const maintenance = new ServiceMaintenanceRuntime({
      preflight: async () => true,
      publishState: () => undefined,
      forceStop: () => undefined
    });
    for (const unit of work) {
      maintenance.enqueue({
        id: unit.id,
        ready: () => true,
        relocate: unit.relocate
      });
    }
    const operationSignal = signal ?? new AbortController().signal;
    const deadlineMs = signal === undefined ? 30_000 : 24 * 60 * 60 * 1000;
    const result = await raceAbort(
      maintenance.start('retire', deadlineMs, stopStartingSignal, operationSignal),
      operationSignal
    );
    if (result.state !== 'completed') {
      throw result.terminalError ?? new Error(`Host relocation ended in '${result.state}'.`);
    }
  }

  async tryHandleControl(
    meshName: string,
    record: ReceiveRecord,
    signal?: AbortSignal
  ): Promise<boolean> {
    if (record.parts.length !== 1) return false;
    const payload = record.parts[0]!.data();
    if (record.kind === ReceiveKind.NodeSend) {
      const sourceLeave = decodeRemoteActorSourceLeaveTerminal(payload);
      if (sourceLeave !== undefined) {
        return this.acceptActorJoinSourceLeave(sourceLeave);
      }
    }
    if (isServiceWireCommand(payload, M6bServiceWireCommand.sessionRelocationSealed)) {
      this.acceptSessionRelocationResponse(
        decodeSessionRelocationSealed(payload),
        record.sourceNodeRid
      );
      return true;
    }
    if (isServiceWireCommand(payload, M6bServiceWireCommand.sessionRelocationSeal)) {
      const request = decodeSessionRelocationSeal(payload);
      const response = await this.handleSessionRelocationSeal(
        meshName,
        request,
        record.sourceNodeRid,
        signal
      );
      await this.sendSessionRelocationResponse(
        meshName,
        record.sourceNodeRid,
        encodeSessionRelocationSealed(response)
      );
      return true;
    }
    if (isServiceWireCommand(payload, M6bServiceWireCommand.sessionRelocationRoute)) {
      const request = decodeSessionRelocationRoute(payload);
      await this.handleSessionRelocationRoute(
        meshName,
        request,
        record.sourceNodeRid,
        signal
      );
      return true;
    }
    if (isServiceWireCommand(payload, M6bServiceWireCommand.replyRelay)) {
      await this.handleReplyRelay(
        meshName,
        decodeMaintenanceReplyRelay(payload),
        record.sourceNodeRid,
        signal
      );
      return true;
    }
    if (isServiceWireCommand(payload, M6bServiceWireCommand.replyRelayAck)) {
      await this.acceptReplyRelayAck(
        meshName,
        decodeMaintenanceReplyRelayAck(payload),
        record.sourceNodeRid,
        signal
      );
      return true;
    }
    const request = decodeServiceRelocationControlRequest(payload);
    if (request === undefined) return false;
    if (request.kind === 'ready') {
      this.acceptControlResponse(request, record.sourceNodeRid);
      return true;
    }
    if (request.kind === 'prepare') {
      if (record.sourceNodeRid === null) {
        throw new Error('Relocation prepare has no authenticated source node.');
      }
      const operationKey = relocationStagingId(request);
      const fingerprint = stringifyWire(request);
      let operation = this.targetPrepareOperations.get(operationKey);
      if (operation !== undefined && operation.fingerprint !== fingerprint) {
        throw new Error(`Relocation '${operationKey}' repeated Prepare with different bytes.`);
      }
      if (operation === undefined) {
        const promise = this.handlePrepareControl(
          meshName,
          operationKey,
          request,
          record.sourceNodeRid,
          signal
        );
        operation = { fingerprint, promise };
        this.targetPrepareOperations.set(operationKey, operation);
        void promise.finally(() => {
          if (this.targetPrepareOperations.get(operationKey) === operation) {
            this.targetPrepareOperations.delete(operationKey);
          }
        }).catch(() => undefined);
      }
      const response = await operation.promise;
      try {
        const submitted = await this.requireMeshNode(meshName).sendToNode(
          record.sourceNodeRid,
          encodeServiceRelocationControlResponse(response)
        );
        if (submitted !== SubmitResult.Ok) {
          throw new Error('Relocation Ready reply was not accepted by RouteMesh.');
        }
      } catch (error) {
        await this.rollbackTargetReadyDelivery(operationKey, error);
        throw error;
      }
      this.armTargetCutoverFallback(meshName, operationKey);
      return true;
    }
    await this.handleOneWayControl(meshName, request, record.sourceNodeRid, signal);
    return true;
  }

  requestSessionRelocationSeal(
    meshName: string,
    targetNodeRid: RoutingId,
    request: ServiceSessionRelocationSeal,
    signal?: AbortSignal
  ): Promise<ServiceSessionRelocationSealed> {
    return this.requestSessionRelocationSealControl(
      meshName,
      targetNodeRid,
      request,
      signal);
  }

  async sendSessionRelocationRoute(
    meshName: string,
    targetNodeRid: RoutingId,
    request: ServiceSessionRelocationRoute,
    signal?: AbortSignal
  ): Promise<void> {
    //  A relocation whose session owner IS this node publishes the route
    //  to itself; RouteMesh has no self connection (NotConnected), so the
    //  control dispatches through the same inbound handler locally.
    const localRid =
      this.requireMeshNode(meshName).status?.().routingId ?? null;
    if (localRid !== null && String(localRid) === String(targetNodeRid)) {
      await this.handleSessionRelocationRoute(
        meshName,
        request,
        localRid,
        signal
      );
      return;
    }
    //  Spec 32:87 — a route/connection that is not usable yet is the
    //  retryable Unavailable class. A one-shot submit rejection here loses
    //  the post-commit route publication (command 44), so the sealed
    //  session is never released and dies on the seal timeout instead.
    //  Retry the identical bytes within the seal window; identical-byte
    //  resends of this one-way control are the same idempotent pattern
    //  the command 42 loop uses.
    const bytes = encodeSessionRelocationRoute(request);
    const deadline = Date.now() + 2_500;
    for (;;) {
      signal?.throwIfAborted();
      const submitted = await this.requireMeshNode(meshName).sendToNode(
        targetNodeRid,
        bytes
      );
      if (submitted === SubmitResult.Ok) return;
      if (Date.now() > deadline) {
        throw createInternalFrameworkException(
          ZLinkFrameworkInternalErrorKind.RelocationTargetUnavailable,
          'Session relocation route was not accepted by RouteMesh'
            + ` (submit=${String(submitted)}`
            + ` target=${String(targetNodeRid)}).`,
          true
        );
      }
      await new Promise(resolve => setTimeout(resolve, 100));
    }
  }

  private requestSessionRelocationSealControl(
    meshName: string,
    targetNodeRid: RoutingId,
    request: ServiceSessionRelocationSeal,
    signal?: AbortSignal
  ): Promise<ServiceSessionRelocationSealed> {
    const key = sessionRelocationPendingKey(request);
    const bytes = encodeSessionRelocationSeal(request);
    const requestFingerprint = bytes.toString('base64');
    const expectedTarget = String(targetNodeRid);
    const terminal = this.terminalSessionRelocations.get(key);
    if (terminal !== undefined) {
      if (
        terminal.targetNodeRid !== expectedTarget
        || terminal.requestFingerprint !== requestFingerprint
      ) {
        return Promise.reject(new ServiceWireProtocolError(
          `Session relocation control '${key}' repeated with different bytes or target.`
        ));
      }
      this.terminalSessionRelocations.touch(key);
    }
    const existing = this.pendingSessionRelocations.get(key);
    if (existing !== undefined) {
      if (
        existing.targetNodeRid !== expectedTarget
        || existing.requestFingerprint !== requestFingerprint
      ) {
        return Promise.reject(new ServiceWireProtocolError(
          `Session relocation control '${key}' repeated with different bytes or target.`
        ));
      }
      return existing.promise;
    }

    let resolvePromise!: (response: ServiceSessionRelocationSealed) => void;
    let rejectPromise!: (error: unknown) => void;
    const promise = new Promise<ServiceSessionRelocationSealed>(
      (resolve, reject) => {
        resolvePromise = resolve;
        rejectPromise = reject;
      }
    );
    let abortListener: (() => void) | undefined;
    let settled = false;
    const finish = (action: () => void) => {
      if (settled) return;
      settled = true;
      const current = this.pendingSessionRelocations.get(key);
      if (current === pending) {
        if (current.retryTimer !== undefined) clearTimeout(current.retryTimer);
        if (current.deadlineTimer !== undefined) clearTimeout(current.deadlineTimer);
        this.pendingSessionRelocations.delete(key);
      }
      if (abortListener !== undefined) {
        signal?.removeEventListener('abort', abortListener);
      }
      action();
    };
    let pending!: PendingSessionRelocation;
    // Spec 48:205/20:408 — the seal control keeps the operation's deadline
    // and cancellation contract; an unbounded identical-bytes resend loop
    // turned a lost ACK into a permanent relocation stall (observed ~30s
    // admission interruption until teardown). Resends stay within the
    // relocation seal window, then the join terminates DeadlineExceeded
    // (spec 32:88).
    // Spec 06 — the configured SessionRelocationSealTimeout is an additional
    // finite upper bound; the caller's earlier absolute deadline still wins.
    const sealTimeoutMs = this.options.registration?.locations.options
      .sessionRelocationSealTimeoutMs
      ?? zlinkRuntimeDefaultLocationOptions.sessionRelocationSealTimeoutMs;
    const resendDeadline = Date.now() + sealTimeoutMs;
    const send = async (): Promise<void> => {
      if (signal?.aborted === true) {
        finish(() => rejectPromise(signal.reason));
        return;
      }
      //  A seal whose session owner IS this node has no RouteMesh self
      //  connection (NotConnected); dispatch through the inbound handler
      //  locally instead of the wire.
      const localRid =
        this.requireMeshNode(meshName).status?.().routingId ?? null;
      if (localRid !== null && String(localRid) === expectedTarget) {
        try {
          const response = await this.handleSessionRelocationSeal(
            meshName,
            request,
            localRid,
            signal
          );
          pending.resolve(response);
        } catch (error) {
          pending.reject(error);
        }
        return;
      }
      if (Date.now() >= resendDeadline) {
        pending.reject(createInternalFrameworkException(
          ZLinkFrameworkInternalErrorKind.DeadlineExceeded,
          `Session relocation seal '${key}' was not acknowledged within the relocation seal timeout.`,
          true
        ));
        return;
      }
      try {
        const submitted = await this.requireMeshNode(meshName)
          .sendToNode(targetNodeRid, bytes);
        if (submitted !== SubmitResult.Ok) {
          pending.reject(createInternalFrameworkException(
            ZLinkFrameworkInternalErrorKind.RelocationTargetUnavailable,
            `Session relocation seal '${key}' was not accepted by RouteMesh.`,
            true
          ));
          return;
        }
        if (this.pendingSessionRelocations.get(key) === pending) {
          pending.retryTimer = setTimeout(() => void send(), 250);
        }
      } catch (error) {
        pending.reject(createInternalFrameworkException(
          ZLinkFrameworkInternalErrorKind.RelocationTargetUnavailable,
          `Session relocation seal '${key}' could not reach its RouteMesh owner.`,
          true,
          error
        ));
      }
    };
    pending = {
      targetNodeRid: expectedTarget,
      request,
      requestFingerprint,
      promise,
      resolve: response => finish(() => {
        this.terminalSessionRelocations.remember(key, {
          targetNodeRid: expectedTarget,
          request,
          requestFingerprint,
          responseFingerprint: sessionRelocationResponseFingerprint(response)
        });
        resolvePromise(response);
      }),
      reject: error => finish(() => rejectPromise(error))
    };
    this.pendingSessionRelocations.set(key, pending);
    pending.deadlineTimer = setTimeout(() => pending.reject(
      createInternalFrameworkException(
        ZLinkFrameworkInternalErrorKind.DeadlineExceeded,
        `Session relocation seal '${key}' was not acknowledged within the relocation seal timeout.`,
        true
      )
    ), Math.max(0, resendDeadline - Date.now()));
    pending.deadlineTimer.unref?.();
    abortListener = () => pending.reject(
      signal?.reason ?? createInternalFrameworkException(
        ZLinkFrameworkInternalErrorKind.DeadlineExceeded,
        `Session relocation seal '${key}' was cancelled.`,
        true
      )
    );
    if (signal?.aborted === true) {
      abortListener();
      return promise;
    }
    signal?.addEventListener('abort', abortListener, { once: true });
    void send();
    return promise;
  }

  private acceptSessionRelocationResponse(
    response: ServiceSessionRelocationSealed,
    sourceNodeRid: RoutingId | null
  ): void {
    const key = sessionRelocationResponseKey(response);
    const pending = this.pendingSessionRelocations.get(key);
    if (pending === undefined) {
      const terminal = this.terminalSessionRelocations.get(key);
      if (terminal === undefined) return;
      if (sourceNodeRid === null || String(sourceNodeRid) !== terminal.targetNodeRid) {
        throw new ServiceWireProtocolError(
          `Session relocation ACK '${key}' source node changed.`
        );
      }
      validateSessionRelocationResponse(terminal.request, response);
      if (sessionRelocationResponseFingerprint(response) !== terminal.responseFingerprint) {
        throw new ServiceWireProtocolError(
          `Session relocation ACK '${key}' repeated with different bytes.`
        );
      }
      this.terminalSessionRelocations.touch(key);
      return;
    }
    if (sourceNodeRid === null || String(sourceNodeRid) !== pending.targetNodeRid) {
      pending.reject(new ServiceWireProtocolError(
        `Session relocation ACK '${key}' source node changed.`
      ));
      return;
    }
    try {
      validateSessionRelocationResponse(pending.request, response);
      pending.resolve(response);
    } catch (error) {
      pending.reject(error);
    }
  }

  //  A relocation control dispatched locally names THIS node as its
  //  source; the peers() view never contains the local node, so its
  //  lifecycle generation comes from the node status instead.
  private sourceLifecycleGeneration(
    meshName: string,
    sourceNodeRid: RoutingId | null
  ): bigint | undefined {
    if (sourceNodeRid === null) return undefined;
    const node = this.requireMeshNode(meshName);
    const status = node.status?.();
    if (status?.routingId !== null && status !== undefined
        && String(status.routingId) === String(sourceNodeRid)) {
      return status.lifecycleGeneration;
    }
    return node.peers().find(
      peer => peer.routingId !== null
        && String(peer.routingId) === String(sourceNodeRid)
    )?.lifecycleGeneration;
  }

  private async handleSessionRelocationSeal(
    meshName: string,
    request: ServiceSessionRelocationSeal,
    sourceNodeRid: RoutingId | null,
    signal?: AbortSignal
  ): Promise<ServiceSessionRelocationSealed> {
    if (
      sourceNodeRid === null
      || String(sourceNodeRid) !== request.coordinator.nodeRid
      || String(sourceNodeRid) !== request.actor.actor.nodeRid
    ) {
      throw new ServiceWireProtocolError(
        'Session relocation seal source does not match its coordinator and Actor fence.'
      );
    }
    this.validateSessionOwnerFence(meshName, request.session);
    const sealSourceGeneration =
      this.sourceLifecycleGeneration(meshName, sourceNodeRid);
    if (sealSourceGeneration !== request.actor.targetNodeGeneration
      || sealSourceGeneration !== request.coordinator.nodeGeneration) {
      throw new ServiceWireProtocolError(
        'Session relocation seal source lifecycle generation is stale.'
      );
    }
    const handler = this.options.boundSessionRelocation?.receiveSeal;
    if (handler === undefined) {
      throw new Error('Session relocation seal ingress is not configured.');
    }
    return await handler(request, signal);
  }

  private async handleSessionRelocationRoute(
    meshName: string,
    request: ServiceSessionRelocationRoute,
    sourceNodeRid: RoutingId | null,
    _signal?: AbortSignal
  ): Promise<void> {
    this.validateSessionOwnerFence(meshName, request.session);
    const expectedSource = request.route.action === 'commit'
      ? request.route.targetNodeRid
      : request.coordinator.nodeRid;
    if (sourceNodeRid === null || String(sourceNodeRid) !== expectedSource) {
      throw new ServiceWireProtocolError(
        'Session relocation route source does not match its action fence.'
      );
    }
    const routeSourceGeneration =
      this.sourceLifecycleGeneration(meshName, sourceNodeRid);
    const expectedSourceGeneration = request.route.action === 'commit'
      ? request.route.targetNodeGeneration
      : request.coordinator.nodeGeneration;
    if (routeSourceGeneration !== expectedSourceGeneration) {
      throw new ServiceWireProtocolError(
        'Session relocation route source lifecycle generation is stale.'
      );
    }
    const handler = this.options.boundSessionRelocation?.receiveRoute;
    if (handler === undefined) {
      throw new Error('Session relocation route ingress is not configured.');
    }
    await handler(request);
  }

  private validateSessionOwnerFence(
    meshName: string,
    session: import('../foundation/service-stateful-wire-codec').ServiceSessionRelocationOwnerFence
  ): void {
    const status = this.requireMeshNode(meshName).status();
    const owner = this.options.currentOwner();
    if (
      String(status.routingId) !== session.sessionOwnerNodeRid
      || status.lifecycleGeneration !== session.sessionOwnerNodeGeneration
      || owner?.ownerId !== session.sessionOwnerId
      || owner.leaseGeneration !== session.sessionOwnerLeaseGeneration
    ) {
      throw new ServiceWireProtocolError(
        'Session relocation command does not match the local Session owner fence.'
      );
    }
  }

  private async sendSessionRelocationResponse(
    meshName: string,
    targetNodeRid: RoutingId | null,
    bytes: Uint8Array
  ): Promise<void> {
    if (targetNodeRid === null) {
      throw new ServiceWireProtocolError('Session relocation command has no authenticated source.');
    }
    const submitted = await this.requireMeshNode(meshName).sendToNode(targetNodeRid, bytes);
    if (submitted !== SubmitResult.Ok) {
      throw new Error('Session relocation ACK was not accepted by RouteMesh.');
    }
  }

  private async relocateSpotAggregate(
    meshName: string,
    activation: ZLinkSpotActivation,
    kind: 'user_spot' | 'instance_spot',
    actorStates: readonly ZLinkActorRuntimeState[],
    target: ZLinkMeshNodeDescriptor | undefined,
    targetApplicationVersion: bigint | undefined,
    signal?: AbortSignal
  ): Promise<void> {
    const store = this.requireLocationStore();
    const spotKey = encodeAuthorityKey(kind, String(activation.spotId));
    const spotAuthority = await requireAuthority(store, spotKey, signal);
    const actorAuthorities = new Map<string, ZLinkAuthoritySnapshot>();
    for (const state of actorStates) {
      actorAuthorities.set(
        state.actorId,
        await requireAuthority(store, encodeAuthorityKey('actor', state.actorId), signal)
      );
    }
    const sessions: SourceActorSession[] = [];
    let spotCapture: Awaited<ReturnType<ZLinkSpotActivation['captureRelocation']>> | undefined;
    let spotMessageFollowSeal: ServiceSpotMessageFollowSeal | undefined;
    let spotSealCommitted = false;
    let readinessBoundaryConsumed = false;
    let interruptionStartedAt: number | undefined;
    const spotRegistration = this.spotRegistration(meshName, kind, spotAuthority.allocation.stableType);
    target ??= await this.selectTarget(
      meshName,
      String(this.requireMeshNode(meshName).status().routingId),
      targetApplicationVersion,
      relocationTargetRequirements([
        {
          objectKind: kind,
          stableType: spotAuthority.allocation.stableType,
          policy: spotRegistration.relocation.kind,
          count: 1
        },
        ...actorStates.map(state => {
          const actorAuthority = actorAuthorities.get(state.actorId)!;
          return {
            objectKind: 'actor' as const,
            stableType: actorAuthority.allocation.stableType,
            policy: this.actorRegistration(
              state.meshName ?? meshName,
              actorAuthority.allocation.stableType
            ).relocation.kind,
            count: 1
          };
        })
      ]),
      signal
    );
    const aggregateId = this.reserveRelocationId();
    const spotUnit: ServiceRelocationCaptureUnit = {
      authorityKey: spotKey.value,
      objectKind: kind,
      stableType: spotAuthority.allocation.stableType,
      objectGeneration: spotAuthority.objectGeneration,
      authorityOwnerGeneration: spotAuthority.authorityOwnerGeneration,
      seal: async captureSignal => {
        interruptionStartedAt = performance.now();
        spotMessageFollowSeal = this.sealSpotMessageFollow(
          meshName,
          spotAuthority,
          activation
        );
        // captureRelocation seals synchronously before its first await. Invoke
        // it in the same event-loop turn as the wire ingress seal so no
        // accepted direct message can enter between the two boundaries.
        const spotCaptureOperation = activation.captureRelocation(captureSignal);
        const preparedSessions = Promise.all(actorStates.map(async state => ({
            state,
            actor: state.actor!,
            prepared: await this.options.actorTransfer.prepareMaintenanceSession(
              state.actor!, state, captureSignal, false, relocationWireId(aggregateId))
          })));
        const preparedSessionsOutcome = preparedSessions.then(
          value => ({ value }),
          error => ({ error })
        );
        spotCapture = await spotCaptureOperation;
        const preparation = await preparedSessionsOutcome;
        if ('error' in preparation) throw preparation.error;
        sessions.push(...preparation.value);
        return {
          boundSessionState: Buffer.alloc(0),
          queuedMessages: [],
          timers: spotCapture.timers.map(toServiceTimer)
        };
      },
      captureApplicationState: captureSignal =>
        this.captureApplication(spotRegistration.relocation, activation.spot, captureSignal),
      commitSeal: async () => {
        if (!spotSealCommitted) {
          if (spotCapture === undefined || !await activation.commitRelocation(spotCapture)) {
            throw new Error(`Spot '${String(activation.spotId)}' relocation seal became stale.`);
          }
          spotSealCommitted = true;
        }
        if (spotMessageFollowSeal !== undefined) {
          await this.commitSpotMessageFollow(
            meshName,
            spotMessageFollowSeal,
            spotKey,
            activation,
            target
          );
        }
        for (const session of sessions) {
          const committedAuthority = await requireAuthority(
            this.requireLocationStore(),
            encodeAuthorityKey('actor', session.state.actorId)
          );
          const targetActorRef = {
            actorId: session.state.actorId,
            objectGeneration: actorAuthorities.get(session.state.actorId)!.objectGeneration,
            meshName,
            nodeRid: target.rid
          };
          await session.prepared.commit({
            routerChannelId: meshName,
            targetNodeRid: target.rid,
            spotId: activation.spotId,
            spotKind: kind === 'user_spot'
              ? ZLinkSpotKind.User
              : ZLinkSpotKind.Instance,
            authorityOwnerGeneration: committedAuthority.authorityOwnerGeneration
          } as never, targetActorRef, committedActorOwnerFence(
            session.state.actorId,
            targetActorRef,
            committedAuthority
          ));
          activation.commitActorDeparture(session.state.actorId);
          await this.requireActorManager().completeRelocationSource(session.state.actorId);
        }
        await this.requireSpotManager().completeRelocationSource(activation);
      },
      abortSeal: async () => {
        const failures: unknown[] = [];
        if (spotCapture !== undefined) {
          try {
            activation.abortRelocation(spotCapture);
          } catch (error) {
            failures.push(error);
          }
        }
        if (spotMessageFollowSeal !== undefined) {
          try {
            const node = this.requireMeshNode(meshName);
            const abort = node.abortSpotMessageFollowIngress;
            if (abort === undefined || !abort.call(node, spotMessageFollowSeal)) {
              throw new Error('Spot Message Follow abort fence became stale.');
            }
          } catch (error) {
            failures.push(error);
          }
        }
        for (const session of [...sessions].reverse()) {
          try {
            await session.prepared.rollback();
          } catch (error) {
            failures.push(error);
          }
        }
        if (failures.length !== 0) {
          throw new AggregateError(failures, 'Spot relocation source rollback was incomplete.');
        }
      }
    };
    const actorUnits = actorStates.map(state => this.actorCaptureUnit(
      state,
      actorAuthorities.get(state.actorId)!,
      sessions
    ));
    const memberships = actorStates.map(state => ({
      actorKey: encodeAuthorityKey('actor', state.actorId).value,
      spotKey: spotKey.value,
      spotObjectGeneration: spotAuthority.objectGeneration,
      membershipEpoch: state.spotMembershipEpoch > 0n ? state.spotMembershipEpoch : 1n
    }));
    let outcome = 'completed';
    try {
      const waitForRelocationBoundary = (
        activation as unknown as {
          waitForRelocationBoundary?: (signal?: AbortSignal) => Promise<boolean>;
        }
      ).waitForRelocationBoundary;
      readinessBoundaryConsumed = waitForRelocationBoundary === undefined
        ? false
        : await waitForRelocationBoundary.call(activation, signal);
      const captureOwner = new ServiceRelocationObjectCaptureOwner();
      const captured = kind === 'user_spot'
        ? await captureOwner.captureUserSpotAggregate(
            aggregateId, 1n, spotUnit, actorUnits, memberships, signal)
        : await captureOwner.captureInstanceSpotAggregate(
            aggregateId, 1n, spotUnit, actorUnits, memberships, signal);
      await this.runCoordinator(
        meshName,
        target,
        spotAuthority,
        captured,
        new Map([[spotKey.value, spotAuthority], ...actorStates.map(state => [
          encodeAuthorityKey('actor', state.actorId).value,
          actorAuthorities.get(state.actorId)!
        ] as const)]),
        signal,
        sessions
      );
    } catch (error) {
      outcome = 'failed';
      if (
        readinessBoundaryConsumed
        && !(error instanceof ServiceRelocationPostCommitError)
      ) {
        await activation.completeConsumedRelocationBoundary(
          ZLinkSpotRelocationReadyOutcome.Continued
        ).catch(() => undefined);
      }
      throw error;
    } finally {
      this.sourceRelocationIds.delete(aggregateId);
      if (interruptionStartedAt !== undefined) {
        this.recordRelocationInterruption(
          meshName,
          kind,
          kind === 'user_spot' ? 'spot_wide' : undefined,
          interruptionStartedAt,
          outcome
        );
      }
    }
  }

  private async relocateStandaloneActor(
    meshName: string,
    state: ZLinkActorRuntimeState,
    target: ZLinkMeshNodeDescriptor | undefined,
    targetApplicationVersion: bigint | undefined,
    signal?: AbortSignal,
    targetMembership?: {
      readonly spotId: RoutingId;
      readonly spotKind: ZLinkSpotKind.Entry | ZLinkSpotKind.User;
      readonly spotObjectGeneration: bigint;
    },
    aggregateIdOverride?: string,
    retainSourceForActorJoin = false
  ): Promise<void> {
    const authority = await requireAuthority(
      this.requireLocationStore(),
      encodeAuthorityKey('actor', state.actorId),
      signal
    );
    target ??= await this.selectTarget(
      meshName,
      String(this.requireMeshNode(meshName).status().routingId),
      targetApplicationVersion,
      [{
        objectKind: 'actor',
        stableType: authority.allocation.stableType,
        policy: this.actorRegistration(
          state.meshName ?? meshName,
          authority.allocation.stableType
        ).relocation.kind,
        count: 1
      }],
      signal
    );
    const sessions: SourceActorSession[] = [];
    const membershipSpotId = targetMembership?.spotId
      ?? ((target.entrySpotId ?? String(target.rid)) as RoutingId);
    const aggregateId = aggregateIdOverride ?? this.reserveRelocationId();
    const unit = this.actorCaptureUnit(
      state,
      authority,
      sessions,
      true,
      target,
      meshName,
      {
        spotId: membershipSpotId,
        spotKind: targetMembership?.spotKind ?? ZLinkSpotKind.Entry
      },
      relocationWireId(aggregateId),
      retainSourceForActorJoin
    );
    const membership: ServiceRelocationMembership = {
      actorKey: encodeAuthorityKey('actor', state.actorId).value,
      spotKey: encodeAuthorityKey('user_spot', String(membershipSpotId)).value,
      spotObjectGeneration:
        targetMembership?.spotObjectGeneration ?? target.lifecycleGeneration,
      membershipEpoch: state.spotMembershipEpoch > 0n ? state.spotMembershipEpoch : 1n
    };
    let interruptionStartedAt: number | undefined;
    const measuredUnit: ServiceRelocationCaptureUnit = {
      ...unit,
      seal: async captureSignal => {
        interruptionStartedAt = performance.now();
        return unit.seal(captureSignal);
      }
    };
    let outcome = 'completed';
    try {
      const captured = await new ServiceRelocationObjectCaptureOwner().captureStandaloneActor(
        aggregateId, 1n, measuredUnit, membership, signal);
      await this.runCoordinator(
        meshName,
        target,
        authority,
        captured,
        new Map([[measuredUnit.authorityKey, authority]]),
        signal,
        sessions
      );
    } catch (error) {
      outcome = 'failed';
      throw error;
    } finally {
      this.sourceRelocationIds.delete(aggregateId);
      if (interruptionStartedAt !== undefined) {
        this.recordRelocationInterruption(
          meshName,
          'actor',
          undefined,
          interruptionStartedAt,
          outcome
        );
      }
    }
  }

  private async relocatePerActorSpotShell(
    meshName: string,
    activation: ZLinkSpotActivation,
    target: ZLinkMeshNodeDescriptor | undefined,
    targetApplicationVersion: bigint | undefined,
    signal?: AbortSignal
  ): Promise<void> {
    const spotKey = encodeAuthorityKey('user_spot', String(activation.spotId));
    const spotAuthority = await requireAuthority(
      this.requireLocationStore(),
      spotKey,
      signal
    );
    const spotRegistration = this.spotRegistration(
      meshName,
      'user_spot',
      spotAuthority.allocation.stableType
    );
    target ??= await this.selectTarget(
      meshName,
      String(this.requireMeshNode(meshName).status().routingId),
      targetApplicationVersion,
      [{
        objectKind: 'user_spot',
        stableType: spotAuthority.allocation.stableType,
        policy: spotRegistration.relocation.kind,
        count: 1
      }],
      signal
    );
    let spotCapture:
      Awaited<ReturnType<ZLinkSpotActivation['captureRelocation']>> | undefined;
    let spotMessageFollowSeal: ServiceSpotMessageFollowSeal | undefined;
    let sealCommitted = false;
    let interruptionStartedAt: number | undefined;
    const spotUnit: ServiceRelocationCaptureUnit = {
      authorityKey: spotKey.value,
      objectKind: 'user_spot',
      stableType: spotAuthority.allocation.stableType,
      objectGeneration: spotAuthority.objectGeneration,
      authorityOwnerGeneration: spotAuthority.authorityOwnerGeneration,
      seal: async captureSignal => {
        interruptionStartedAt = performance.now();
        spotMessageFollowSeal = this.sealSpotMessageFollow(
          meshName,
          spotAuthority,
          activation
        );
        spotCapture = await activation.captureRelocation(captureSignal);
        return {
          boundSessionState: Buffer.alloc(0),
          queuedMessages: [],
          timers: spotCapture.timers.map(toServiceTimer)
        };
      },
      // PerActor User Spots are stateless shells. Application state and the
      // Spot relocation adapter are intentionally not part of this unit.
      captureApplicationState: async () => Buffer.alloc(0),
      commitSeal: async () => {
        if (!sealCommitted) {
          if (spotCapture === undefined || !await activation.commitRelocation(spotCapture)) {
            throw new Error(
              `PerActor Spot '${String(activation.spotId)}' relocation seal became stale.`
            );
          }
          sealCommitted = true;
        }
        if (spotMessageFollowSeal !== undefined) {
          await this.commitSpotMessageFollow(
            meshName,
            spotMessageFollowSeal,
            spotKey,
            activation,
            target
          );
        }
        await this.requireSpotManager().completeRelocationSource(activation);
      },
      abortSeal: async () => {
        if (spotCapture !== undefined) activation.abortRelocation(spotCapture);
        if (spotMessageFollowSeal !== undefined) {
          const node = this.requireMeshNode(meshName);
          const abort = node.abortSpotMessageFollowIngress;
          if (abort === undefined || !abort.call(node, spotMessageFollowSeal)) {
            throw new Error('Spot Message Follow abort fence became stale.');
          }
        }
      }
    };
    const aggregateId = this.reserveRelocationId();
    let outcome = 'completed';
    try {
      const captured = await new ServiceRelocationObjectCaptureOwner().captureUserSpotAggregate(
        aggregateId,
        1n,
        spotUnit,
        [],
        [],
        signal
      );
      await this.runCoordinator(
        meshName,
        target,
        spotAuthority,
        captured,
        new Map([[spotKey.value, spotAuthority]]),
        signal,
        []
      );
    } catch (error) {
      outcome = 'failed';
      throw error;
    } finally {
      this.sourceRelocationIds.delete(aggregateId);
      if (interruptionStartedAt !== undefined) {
        this.recordRelocationInterruption(
          meshName,
          'user_spot',
          'per_actor',
          interruptionStartedAt,
          outcome
        );
      }
    }
  }

  private actorCaptureUnit(
    state: ZLinkActorRuntimeState,
    authority: ZLinkAuthoritySnapshot,
    sessions: SourceActorSession[],
    standalone = false,
    target?: ZLinkMeshNodeDescriptor,
    meshName?: string,
    targetMembership?: {
      readonly spotId: RoutingId;
      readonly spotKind: ZLinkSpotKind.Entry | ZLinkSpotKind.User;
    },
    sessionRelocation?: ServiceWireOperationId,
    retainSourceForActorJoin = false
  ): ServiceRelocationCaptureUnit {
    const registration = this.actorRegistration(state.meshName ?? meshName ?? '', state.actorType ?? authority.allocation.stableType);
    let ownSession: SourceActorSession | undefined;
    return {
      authorityKey: encodeAuthorityKey('actor', state.actorId).value,
      objectKind: 'actor',
      stableType: authority.allocation.stableType,
      objectGeneration: authority.objectGeneration,
      authorityOwnerGeneration: authority.authorityOwnerGeneration,
      seal: async signal => {
        if (standalone) {
          ownSession = {
            state,
            actor: state.actor!,
            prepared: await this.options.actorTransfer.prepareMaintenanceSession(
              state.actor!, state, signal, true, sessionRelocation)
          };
          sessions.push(ownSession);
        }
        const prepared = standalone ? ownSession : sessions.find(value => value.state === state);
        return {
          boundSessionState: encodeActorSession(prepared?.prepared.target),
          queuedMessages: encodeHandoffQueuedMessages(prepared?.prepared.handoffBacklog ?? []),
          timers: []
        };
      },
      captureApplicationState: signal =>
        this.captureApplication(registration.relocation, state.actor!, signal),
      commitSeal: async () => {
        if (!standalone || ownSession === undefined || target === undefined || meshName === undefined) return;
        const membershipSpotId = targetMembership?.spotId
          ?? ((target.entrySpotId ?? String(target.rid)) as RoutingId);
        const committedAuthority = await requireAuthority(
          this.requireLocationStore(),
          encodeAuthorityKey('actor', state.actorId)
        );
        const targetActorRef = {
          actorId: state.actorId,
          objectGeneration: authority.objectGeneration,
          meshName,
          nodeRid: target.rid
        };
        await ownSession.prepared.commit({
          routerChannelId: meshName,
          targetNodeRid: target.rid,
          spotId: membershipSpotId,
          spotKind: targetMembership?.spotKind ?? ZLinkSpotKind.Entry,
          authorityOwnerGeneration: committedAuthority.authorityOwnerGeneration
        } as never, targetActorRef, committedActorOwnerFence(
          state.actorId,
          targetActorRef,
          committedAuthority
        ));
        if (!retainSourceForActorJoin) {
          await this.requireActorManager().completeRelocationSource(state.actorId);
        }
      },
      abortSeal: async () => {
        if (standalone && ownSession !== undefined) await ownSession.prepared.rollback();
      }
    };
  }

  private reserveRelocationId(): string {
    const id = createServiceRelocationId(candidate => {
      if (this.sourceRelocationIds.has(candidate)) return true;
      const wire = relocationWireId(candidate);
      const prefix = `${wire.high}:${wire.low}:`;
      return [...this.targetStages.keys()].some(key => key.startsWith(prefix));
    });
    this.sourceRelocationIds.add(id);
    return id;
  }

  private reserveExactRelocationId(id: string): void {
    const wire = relocationWireId(id);
    const prefix = `${wire.high}:${wire.low}:`;
    if (this.sourceRelocationIds.has(id)
      || [...this.targetStages.keys()].some(key => key.startsWith(prefix))) {
      throw new Error(`Actor Join relocation '${id}' is already active.`);
    }
    this.sourceRelocationIds.add(id);
  }

  private async runCoordinator(
    meshName: string,
    target: ZLinkMeshNodeDescriptor,
    primary: ZLinkAuthoritySnapshot,
    captured: ServiceCapturedObjectRelocation,
    _authorities: ReadonlyMap<string, ZLinkAuthoritySnapshot>,
    signal?: AbortSignal,
    sessions: readonly SourceActorSession[] = []
  ): Promise<void> {
    const localStatus = this.requireMeshNode(meshName).status();
    const controlDeadlineAtMs = signal === undefined
      ? Date.now() + 30_000
      : Number.MAX_SAFE_INTEGER;
    const coordinator = {
      ownerId: primary.ownerId,
      leaseGeneration: primary.ownerLeaseGeneration,
      nodeRid: String(localStatus.routingId),
      nodeGeneration: localStatus.lifecycleGeneration,
      expectedAuthorityStoreVersion: primary.storeVersion.value
    } satisfies ServiceWireRelocationCoordinatorFence;
    const targetFence = {
      nodeRid: String(target.rid),
      nodeGeneration: target.lifecycleGeneration,
      ownerId: target.ownerId,
      ownerLeaseGeneration: target.leaseGeneration
    } satisfies ServiceWireRelocationTarget;
    const relocation = relocationWireId(captured.envelope.aggregateId);
    const object = relocationObject(captured.envelope);
    const relayAuthorityId = wireIdText(relocationWireId(captured.envelope.aggregateId));
    const relayAuthorityKey = primaryKey(captured.envelope);
    if (this.relocationAuthorityKeys.has(relayAuthorityId)) {
      throw new Error(`Relocation reply relay authority '${relayAuthorityId}' is already registered.`);
    }
    this.relocationAuthorityKeys.set(relayAuthorityId, relayAuthorityKey);
    const encoded = encodeServiceRelocationEnvelope(captured.envelope);
    const relocationStore = relocationStorePort(this.requireRelocationStore());
    let stored: Awaited<ReturnType<typeof relocationStore.put>> | undefined;
    let convergenceDeadlineAtMs = controlDeadlineAtMs;
    let readyReceived = false;
    let sourceCommitted = false;
    try {
      stored = await relocationStore.put(
        encoded,
        RELOCATION_OPERATION_RETENTION_MS,
        signal
      );
      if (stored.checksumCrc32c !== crc32c(encoded)
        || stored.expiresAtMs <= stored.storeNowMs) {
        throw new Error('Relocation Store returned an invalid captured payload receipt.');
      }
      convergenceDeadlineAtMs = Date.now()
        + Math.max(0, stored.expiresAtMs - stored.storeNowMs);
      const prepare = {
        kind: 'prepare',
        relocation,
        targetAttemptGeneration: 1n,
        coordinator,
        target: targetFence,
        initiatorRole: 'source',
        object,
        sourceNodeRid: coordinator.nodeRid,
        sourceNodeGeneration: coordinator.nodeGeneration,
        root: {
          reference: stored.reference,
          checksumCrc32c: stored.checksumCrc32c
        },
        applicationVersion: target.applicationVersion
      } satisfies ServiceMaintenanceRelocationPrepare;
      const ready = await this.sendControl(
        meshName,
        target.rid,
        prepare,
        signal,
        controlDeadlineAtMs
      );
      validateControlResponse(prepare, ready);
      // Ready arms the target's 1,000 ms fallback. From this point forward
      // the source can no longer reopen dispatch without risking two owners.
      readyReceived = true;

      for (const session of sessions) {
        const participant = captured.envelope.participants.find(value =>
          value.objectKind === 'actor'
          && decodeAuthorityKey({ value: value.key } as ZLinkAuthorityKey).globalId
            === session.state.actorId
        );
        if (participant === undefined) {
          throw new Error(`Relocation Actor '${session.state.actorId}' is missing from its envelope.`);
        }
        for (const packet of session.prepared.takeRelocationRelay()) {
          const data = {
            kind: 'data',
            relocation,
            targetAttemptGeneration: 1n,
            coordinator,
            senderRole: 'source',
            object: relocationObjectForParticipant(participant),
            frozenRecord: canonicalRelayFrozenRecord(
              participant,
              packet,
              coordinator,
              targetFence
            )
          } satisfies Extract<ServiceMaintenanceRelocationControl, { kind: 'data' }>;
          await this.sendControlOneWay(meshName, target.rid, data);
        }
      }

      await this.sendControlOneWay(meshName, target.rid, {
        kind: 'cutover',
        relocation,
        targetAttemptGeneration: 1n,
        coordinator,
        senderRole: 'source',
        object
      });

      await this.waitForTargetAuthorities(
        captured.envelope,
        targetFence,
        convergenceDeadlineAtMs,
        signal
      );
      for (const session of sessions) session.prepared.setReplayResults([]);
      await captured.commitSource();
      sourceCommitted = true;
      await this.options.reconcileStatefulAuthorityRoutes?.(signal);
    } catch (error) {
      if (!readyReceived) throw error;
      try {
        await this.waitForTargetAuthorities(
          captured.envelope,
          targetFence,
          convergenceDeadlineAtMs
        );
        if (!sourceCommitted) {
          for (const session of sessions) session.prepared.setReplayResults([]);
          await captured.commitSource();
          sourceCommitted = true;
        }
        await this.options.reconcileStatefulAuthorityRoutes?.();
        const authority = await requireAuthority(
          this.requireLocationStore(),
          { value: primaryKey(captured.envelope) } as ZLinkAuthorityKey,
          undefined
        );
        throw new ServiceRelocationPostCommitError(
          authority,
          { id: `${captured.envelope.aggregateId}:${captured.envelope.aggregateGeneration}` },
          error
        );
      } catch (convergenceError) {
        if (convergenceError instanceof ServiceRelocationPostCommitError) {
          throw convergenceError;
        }
        throw new AggregateError(
          [error, convergenceError],
          'Relocation became irreversible after Ready and target convergence failed.'
        );
      }
    } finally {
      if (!readyReceived) {
        await captured.abortSource().catch(() => undefined);
      }
      if (stored !== undefined) {
        await relocationStore.delete(stored.reference).catch(() => undefined);
      }
      if (this.relocationAuthorityKeys.get(relayAuthorityId) === relayAuthorityKey) {
        this.relocationAuthorityKeys.delete(relayAuthorityId);
      }
    }
  }

  private async releaseParticipantAuthorities(
    envelope: ServiceRelocationEnvelope,
    primaryKey: string,
    signal?: AbortSignal
  ): Promise<void> {
    for (const participant of envelope.participants) {
      if (participant.key === primaryKey) continue;
      const key = { value: participant.key } as ZLinkAuthorityKey;
      const current = await requireAuthority(this.requireLocationStore(), key, signal);
      const publication = this.codec.read(current.payload);
      if (publication === undefined) continue;
      const result = await this.requireLocationStore().compareExchangeAuthority(
        key,
        current.storeVersion,
        {
          kind: 'put',
          generationTransition: 'preserve',
          payload: this.codec.clear(current.payload, publication.reference)
        },
        signal
      );
      if (result.kind !== 'stored') {
        throw new Error(`Participant '${participant.key}' recovery pointer release failed.`);
      }
    }
  }

  /** Restores the hidden object and its saved-work prefix before Ready. */
  private async handlePrepareControl(
    meshName: string,
    stagingId: string,
    request: ServiceMaintenanceRelocationPrepare,
    sourceNodeRid: RoutingId | null,
    signal?: AbortSignal
  ): Promise<ServiceMaintenanceRelocationReady> {
    if (sourceNodeRid === null || String(sourceNodeRid) !== request.sourceNodeRid) {
      throw new Error('Relocation prepare source node fence does not match the authenticated peer.');
    }
    const targetStatus = this.requireMeshNode(meshName).status();
    const targetOwner = this.options.currentOwner();
    const targetDescriptor = this.options.localDescriptor?.(meshName);
    if (targetOwner === undefined
      || String(targetStatus.routingId) !== request.target.nodeRid
      || targetStatus.lifecycleGeneration !== request.target.nodeGeneration
      || targetOwner.ownerId !== request.target.ownerId
      || targetOwner.leaseGeneration !== request.target.ownerLeaseGeneration
      || targetDescriptor !== undefined
        && targetDescriptor.applicationVersion !== request.applicationVersion) {
      throw new Error('Relocation prepare target fence does not match the target owner.');
    }
    if (request.root === undefined) throw new Error('Relocation prepare has no shared durable root.');
    const fingerprint = stringifyWire(request);
    const existing = this.targetStages.get(stagingId);
    if (existing !== undefined) {
      if (existing.offer.authenticatedSourceNodeRid !== String(sourceNodeRid)
        || existing.offer.prepareFingerprint !== fingerprint) {
        throw new Error('Relocation prepare retry source node changed.');
      }
      return relocationReady(request);
    }
    const terminal = this.terminalTargets.get(stagingId);
    if (terminal !== undefined) {
      if (terminal !== fingerprint) {
        throw new Error(`Relocation '${stagingId}' repeated Prepare with different bytes.`);
      }
      this.terminalTargets.touch(stagingId);
      return relocationReady(request);
    }
    const restored = await this.readSharedEnvelope(request.root, signal);
    const envelope = restored.envelope;
    if (restored.expiresAtMs <= Date.now()) {
      throw new Error('Relocation Prepare immutable root has expired.');
    }
    validatePrepareEnvelope(request, envelope);
    const reservation = await this.reserveTargetForPrepare(meshName, request, envelope, signal);
    const offer: TargetRelocationOffer = {
      prepare: request,
      prepareFingerprint: fingerprint,
      authenticatedSourceNodeRid: String(sourceNodeRid),
      envelope,
      restoreDeadlineAtMs: restored.expiresAtMs,
      reservation
    };
    let stage: LocalStage | undefined;
    try {
      stage = await this.materializeTargetOffer(meshName, stagingId, offer, signal);
      await stage.owner.restoreSavedWork(stage.staging, signal);
    } catch (error) {
      if (stage === undefined) {
        await this.abortTargetReservation(reservation, signal).catch(() => undefined);
      } else {
        this.targetStages.delete(stagingId);
        await this.abortTargetStage(stage, signal).catch(() => undefined);
      }
      throw error;
    }
    return relocationReady(request);
  }

  private async rollbackTargetReadyDelivery(
    stagingId: string,
    submitError: unknown
  ): Promise<void> {
    const stage = this.targetStages.get(stagingId);
    if (stage === undefined || stage.phase !== 'ready' || stage.finalize !== undefined) return;
    this.targetStages.delete(stagingId);
    try {
      await this.abortTargetStage(stage);
    } catch (rollbackError) {
      throw new AggregateError(
        [submitError, rollbackError],
        'Relocation Ready delivery and target rollback both failed.'
      );
    }
  }

  private armTargetCutoverFallback(meshName: string, stagingId: string): void {
    const stage = this.targetStages.get(stagingId);
    if (stage === undefined || stage.fallback !== undefined || stage.finalize !== undefined) return;
    stage.fallback = setTimeout(() => {
      void this.beginTargetFinalize(meshName, stagingId, stage, true)
        .catch(error => console.error('[zlink.runtime.relocation.location_update_failed]', error));
    }, 1_000);
    stage.fallback.unref();
  }

  private async handleOneWayControl(
    meshName: string,
    request: ServiceMaintenanceRelocationData | ServiceMaintenanceRelocationCutover,
    sourceNodeRid: RoutingId | null,
    signal?: AbortSignal
  ): Promise<void> {
    if (request.senderRole !== 'source' || sourceNodeRid === null) {
      throw new Error('Relocation one-way control has no exact source peer.');
    }
    const stagingId = relocationStagingId(request);
    const stage = this.targetStages.get(stagingId);
    if (stage === undefined) {
      if (this.terminalTargets.get(stagingId) !== undefined) {
        if (request.kind === 'cutover') {
          console.warn('[zlink.runtime.relocation.late_cutover]', stagingId);
        }
        return;
      }
      throw new Error(`Relocation staging '${stagingId}' is missing.`);
    }
    validateTargetOneWayControl(stage, request, sourceNodeRid);
    if (request.kind === 'data') {
      stage.lane = stage.lane.then(() => this.enqueueRelocationData(stage, request, signal));
      await stage.lane;
      return;
    }
    if (stage.cutoverReceived || stage.phase !== 'ready') {
      console.warn('[zlink.runtime.relocation.late_cutover]', stagingId);
      return;
    }
    stage.cutoverReceived = true;
    if (stage.fallback !== undefined) clearTimeout(stage.fallback);
    await this.beginTargetFinalize(meshName, stagingId, stage, false, signal);
  }

  private beginTargetFinalize(
    meshName: string,
    stagingId: string,
    stage: LocalStage,
    fallback: boolean,
    signal?: AbortSignal
  ): Promise<void> {
    if (stage.finalize !== undefined) return stage.finalize;
    if (fallback) console.warn('[zlink.runtime.relocation.cutover_timeout]', stagingId);
    const finalize = stage.lane.then(() =>
      this.finalizeTargetStage(meshName, stagingId, stage, signal));
    stage.finalize = finalize;
    stage.lane = finalize;
    return finalize;
  }

  private async finalizeTargetStage(
    meshName: string,
    stagingId: string,
    stage: LocalStage,
    signal?: AbortSignal
  ): Promise<void> {
    if (stage.phase !== 'ready') return;
    stage.phase = 'finalizing';
    let authorityCommitted = false;
    let authority: ZLinkAuthoritySnapshot | undefined;
    try {
      authority = await this.commitTargetReservation(
        stage,
        stage.offer.reservation,
        signal
      );
      authorityCommitted = true;
      stage.phase = 'committed';
      await stage.owner.normalize(stage.staging, authority, signal);
      await stage.owner.publish(stage.staging, authority, signal);
      await this.finalizeActorJoinProfiles(meshName, stage, signal);
      await stage.owner.openAdmission(stage.staging, signal);
      await this.publishSessionRoutes(stage.staging);
      await this.relayTerminalReplies(
        meshName,
        stage,
        this.targetReplyRelayCoordinator(meshName, stage, authority),
        signal
      );
      await this.clearTargetRelocationPublication(stage, authority, signal);
      stage.phase = 'open';
      this.terminalTargets.remember(stagingId, stage.offer.prepareFingerprint);
      this.targetStages.delete(stagingId);
    } catch (error) {
      stage.phase = 'failed';
      this.terminalTargets.remember(stagingId, stage.offer.prepareFingerprint);
      this.targetStages.delete(stagingId);
      if (!authorityCommitted) {
        await this.abortTargetStage(stage).catch(() => undefined);
        throw error;
      }
      if (authority !== undefined) {
        throw new ServiceRelocationPostCommitError(authority, stage.staging, error);
      }
      throw error;
    }
  }

  private async enqueueRelocationData(
    stage: LocalStage,
    request: ServiceMaintenanceRelocationData,
    signal?: AbortSignal
  ): Promise<void> {
    const frozen = request.frozenRecord;
    const target = frozen.target;
    const payload = frozen.applicationPayload;
    const preparedTarget = stage.offer.prepare.target;
    if (request.object.kind !== 'actor'
      || target?.kind !== 'actor'
      || payload === undefined
      || payload.packetName !== '__zlink.actor.handoff.accepted'
      || target.actorId !== request.object.actorId
      || target.generation !== request.object.objectGeneration
      || target.targetNodeRid !== preparedTarget.nodeRid
      || target.targetNodeGeneration !== preparedTarget.nodeGeneration
      || target.authorityOwnerGeneration
        !== request.object.expectedAuthorityOwnerGeneration + 1n
      || target.ownerLeaseGeneration !== preparedTarget.ownerLeaseGeneration) {
      throw new Error('Relocation data does not target the exact temporary Actor queue.');
    }
    const key = encodeAuthorityKey('actor', request.object.actorId).value;
    const participant = stage.staging.envelope.participants.find(value => value.key === key);
    if (participant === undefined
      || participant.objectGeneration !== request.object.objectGeneration
      || participant.authorityOwnerGeneration
        !== request.object.expectedAuthorityOwnerGeneration) {
      throw new Error('Relocation data Actor is not part of the prepared unit.');
    }
    await stage.owner.replayRelayedMessage(
      stage.staging,
      key,
      { sequence: 0n, payload: Buffer.from(payload.bytes) },
      signal
    );
  }

  private async finalizeActorJoinProfiles(
    meshName: string,
    stage: LocalStage,
    signal?: AbortSignal
  ): Promise<void> {
    const manager = this.options.spotManager();
    if (manager === undefined) return;
    for (const hidden of stage.staging.hidden.values()) {
      if (hidden.actor === undefined) continue;
      const state = this.options.actorManager()?.getState(hidden.actor.context.actorId);
      const nativeRef = state?.nativeActorRef;
      if (nativeRef === undefined) {
        throw new Error(
          `Actor '${hidden.actor.context.actorId}' relocation target has no native identity.`
        );
      }
      await manager.finalizeActorJoinRelocation(
        meshName,
        stage.staging.envelope.aggregateId,
        hidden.actor,
        toFrameworkActorRef(nativeRef, meshName),
        sourceNodeRid => this.submitActorJoinSourceLeave(
          meshName,
          sourceNodeRid,
          stage.staging.envelope.aggregateId,
          hidden.actor!.context.actorId
        ),
        signal
      );
    }
  }

  private async submitActorJoinSourceLeave(
    meshName: string,
    sourceNodeRid: RoutingId,
    relocationId: string,
    actorId: string
  ): Promise<void> {
    try {
      const submitted = await this.requireMeshNode(meshName).sendToNode(
        sourceNodeRid,
        Buffer.from(JSON.stringify({
          packetName: ZLINK_REMOTE_ACTOR_SOURCE_LEAVE_TERMINAL,
          transferId: relocationId,
          actorId,
          succeeded: true
        }))
      );
      if (submitted !== SubmitResult.Ok) {
        console.warn(
          '[zlink.runtime.relocation.source_leave_submit_failed]',
          actorId,
          submitted
        );
      }
    } catch (error) {
      console.warn(
        '[zlink.runtime.relocation.source_leave_submit_failed]',
        actorId,
        error
      );
    }
  }

  private acceptActorJoinSourceLeave(input: {
    readonly transferId: string;
    readonly actorId: string;
    readonly succeeded: boolean;
  }): boolean {
    const profile = this.sourceActorJoinProfiles.get(input.transferId);
    if (profile === undefined) {
      const terminalActorId = this.terminalActorJoinSourceLeaves.get(input.transferId);
      if (terminalActorId === undefined) return false;
      if (terminalActorId !== input.actorId || !input.succeeded) {
        throw new Error(`Actor Join relocation '${input.transferId}' source leave identity changed.`);
      }
      this.terminalActorJoinSourceLeaves.touch(input.transferId);
      return true;
    }
    if (profile.state.actorId !== input.actorId || !input.succeeded) {
      throw new Error(`Actor Join relocation '${input.transferId}' source leave identity changed.`);
    }
    this.sourceActorJoinProfiles.delete(input.transferId);
    this.terminalActorJoinSourceLeaves.remember(input.transferId, input.actorId);
    void profile.ready.then(async () => {
      await this.options.actorTransfer.completeRelocationSourceLeave(
        profile.actor,
        profile.sourceSpotId
      );
      await this.options.actorManager()?.completeRelocationSource(
        input.actorId,
        profile.sourceActorRef
      );
    }).catch(error => {
      console.error('[zlink.runtime.relocation.source_leave_failed]', error);
    });
    return true;
  }

  private async sendControlOneWay(
    meshName: string,
    targetNodeRid: RoutingId,
    request: ServiceMaintenanceRelocationData | ServiceMaintenanceRelocationCutover
  ): Promise<void> {
    const submitted = await this.requireMeshNode(meshName).sendToNode(
      targetNodeRid,
      encodeServiceRelocationControlRequest(request)
    );
    if (submitted !== SubmitResult.Ok) {
      throw new Error(
        `Relocation ${request.kind === 'data' ? 'relay' : 'cutover'} was not accepted by RouteMesh.`
      );
    }
  }

  private async waitForTargetAuthorities(
    envelope: ServiceRelocationEnvelope,
    target: ServiceWireRelocationTarget,
    deadlineAtMs: number,
    signal?: AbortSignal
  ): Promise<void> {
    for (;;) {
      signal?.throwIfAborted();
      let exact = true;
      for (const participant of envelope.participants) {
        const current = await this.requireLocationStore().readAuthority(
          { value: participant.key } as ZLinkAuthorityKey,
          signal
        );
        if (current.kind !== 'snapshot'
          || current.objectGeneration !== participant.objectGeneration
          || current.ownerId !== target.ownerId
          || current.ownerLeaseGeneration !== target.ownerLeaseGeneration
          || current.authorityOwnerGeneration <= participant.authorityOwnerGeneration
          || String(current.allocation.descriptor.rid) !== target.nodeRid
          || current.allocation.descriptorLifecycleGeneration !== target.nodeGeneration) {
          exact = false;
          break;
        }
      }
      if (exact) return;
      if (Date.now() >= deadlineAtMs) {
        throw new Error('Relocation target owner was not confirmed before its original deadline.');
      }
      await new Promise<void>(resolve => setTimeout(resolve, 10));
    }
  }

  private async clearTargetRelocationPublication(
    stage: LocalStage,
    primary: ZLinkAuthoritySnapshot,
    signal?: AbortSignal
  ): Promise<void> {
    await this.releaseParticipantAuthorities(
      stage.staging.envelope,
      stage.staging.primaryAuthorityKey.value,
      signal
    );
    //  The deferred-Join journal advances its completion cursor with its
    //  own authority CAS while the target finalizes, so the initially
    //  captured storeVersion can be stale by the time the relocation
    //  wrapper is cleared. Re-read and retry on conflict instead of turning
    //  a completed relocation into a post-commit failure (spec 28 —
    //  retryable store conflicts converge on the same CAS and the same
    //  relocation identity until the outcome is determined).
    let current: ZLinkAuthoritySnapshot = primary;
    for (let attempt = 0; attempt < 16; attempt += 1) {
      const publication = this.codec.read(current.payload);
      if (publication === undefined) return;
      const result = await this.requireLocationStore().compareExchangeAuthority(
        stage.staging.primaryAuthorityKey,
        current.storeVersion,
        {
          kind: 'put',
          generationTransition: 'preserve',
          payload: this.codec.clear(current.payload, publication.reference)
        },
        signal
      );
      if (result.kind === 'stored') return;
      const read = await this.requireLocationStore().readAuthority(
        stage.staging.primaryAuthorityKey,
        signal
      );
      if (read.kind !== 'snapshot') break;
      current = read;
    }
    throw new Error('Relocation target authority normalization CAS failed.');
  }

  private async readSharedEnvelope(
    root: NonNullable<ServiceMaintenanceRelocationPrepare['root']>,
    signal?: AbortSignal
  ): Promise<{ readonly envelope: ServiceRelocationEnvelope; readonly expiresAtMs: number }> {
    const read = await readRelocationPayload(
      this.requireRelocationStore(),
      root.reference,
      signal
    );
    if (read.kind === 'missing' || crc32c(read.payload) !== root.checksumCrc32c) {
      throw new Error('Relocation prepare references missing or corrupt shared durable data.');
    }
    return {
      envelope: decodeServiceRelocationEnvelope(read.payload),
      expiresAtMs: Date.now() + Math.max(0, read.expiresAtMs - read.storeNowMs)
    };
  }

  private async handleReplyRelay(
    meshName: string,
    relay: ServiceMaintenanceReplyRelay,
    sourceNodeRid: RoutingId | null,
    signal?: AbortSignal
  ): Promise<void> {
    if (sourceNodeRid === null) {
      throw new Error('Relocation reply relay has no authenticated target node.');
    }
    await this.requireAdmittedPeer(
      meshName,
      String(sourceNodeRid),
      relay.coordinator,
      signal
    );
    const relocationId = wireIdText(relay.relocation);
    const authorityKey = this.relocationAuthorityKeys.get(relocationId);
    if (authorityKey === undefined) {
      throw new Error('Relocation reply relay has no registered durable authority.');
    }
    const authority = await requireAuthority(
      this.requireLocationStore(),
      { value: authorityKey } as ZLinkAuthorityKey,
      signal
    );
    if (authority.storeVersion.value !== relay.coordinator.expectedAuthorityStoreVersion
      || authority.ownerId !== relay.coordinator.ownerId
      || authority.ownerLeaseGeneration !== relay.coordinator.leaseGeneration
      || String(authority.allocation.descriptor.rid) !== relay.coordinator.nodeRid
      || authority.allocation.descriptorLifecycleGeneration
        !== relay.coordinator.nodeGeneration) {
      throw new Error('Relocation reply relay coordinator authority fence is stale.');
    }
    const accepted = this.options.actorTransfer.relayCanonicalMaintenanceTerminal(
      wireIdText(relay.operation),
      relay.replyRouteId.toString(),
      handoffResultFromRelay(relay),
      sourceNodeRid
    );
    if (accepted.status !== 'terminalReceived' && accepted.status !== 'alreadyTerminal'
      || accepted.source === undefined) {
      throw new Error('Canonical relocation reply relay collided with another source route.');
    }
    const requestSource = handoffSourceFence(accepted.source);
    const localStatus = this.requireMeshNode(meshName).status();
    const localOwner = this.options.currentOwner();
    if (localOwner === undefined
      || requestSource.ownerId !== localOwner.ownerId
      || requestSource.leaseGeneration !== localOwner.leaseGeneration
      || !routingIdsEqual(requestSource.nodeRid, localStatus.routingId)
      || requestSource.nodeGeneration !== localStatus.lifecycleGeneration) {
      throw new Error('Relocation reply relay request-source fence changed after capture.');
    }
    const ack: ServiceMaintenanceReplyRelayAck = {
      relocation: relay.relocation,
      coordinator: relay.coordinator,
      operation: relay.operation,
      replyRouteId: relay.replyRouteId,
      requestSource,
      status: accepted.status
    };
    if (await this.requireMeshNode(meshName).sendToNode(
      sourceNodeRid,
      encodeMaintenanceReplyRelayAck(ack)
    ) !== SubmitResult.Ok) {
      throw new Error('Relocation reply relay ACK send was rejected.');
    }
  }

  private async sendReplyRelay(
    meshName: string,
    ackTargetNodeRid: RoutingId,
    request: ServiceMaintenanceReplyRelay,
    expectedRequestSource: ServiceWireRequestSourceFence,
    signal?: AbortSignal
  ): Promise<RelocationTerminalDelivery> {
    const targetRid = String(ackTargetNodeRid);
    const identityKey = replyRelayIdentityKey(request);
    const key = replyRelayPendingKey(targetRid, request);
    if (this.pendingReplyRelays.has(key)) {
      throw new Error(`Relocation reply relay ACK '${key}' is already pending.`);
    }
    const node = this.requireMeshNode(meshName);
    return await new Promise<RelocationTerminalDelivery>((resolve, reject) => {
      let attempts = 0;
      const finish = (action: () => void) => {
        const pending = this.pendingReplyRelays.get(key);
        if (pending?.timer !== undefined) clearTimeout(pending.timer);
        this.pendingReplyRelays.delete(key);
        action();
      };
      const pending: PendingRelocationReplyRelay = {
        ackTargetNodeRid: targetRid,
        identityKey,
        request,
        expectedRequestSource,
        resolve: delivery => finish(() => resolve(delivery)),
        reject: error => finish(() => reject(error))
      };
      const send = async (): Promise<void> => {
        try {
          if (signal?.aborted === true) {
            pending.reject(signal.reason);
            return;
          }
          if (await this.exactSourceLeaseExpired(expectedRequestSource, signal)) {
            pending.resolve('sourceLeaseExpired');
            return;
          }
          if (++attempts > 120) {
            pending.reject(new Error(`Relocation reply relay ACK '${key}' timed out.`));
            return;
          }
          await node.sendToNode(ackTargetNodeRid, encodeMaintenanceReplyRelay(request));
          if (this.pendingReplyRelays.get(key) === pending) {
            pending.timer = setTimeout(() => void send(), 250);
          }
        } catch (error) {
          pending.reject(error);
        }
      };
      this.pendingReplyRelays.set(key, pending);
      void send();
    });
  }

  private async acceptReplyRelayAck(
    meshName: string,
    ack: ServiceMaintenanceReplyRelayAck,
    sourceNodeRid: RoutingId | null,
    signal?: AbortSignal
  ): Promise<void> {
    const identityKey = replyRelayIdentityKey(ack);
    const exactKey = sourceNodeRid === null
      ? undefined
      : replyRelayPendingKey(String(sourceNodeRid), ack);
    const pending = exactKey === undefined ? undefined : this.pendingReplyRelays.get(exactKey);
    if (pending === undefined) {
      const collision = [...this.pendingReplyRelays.values()].find(
        value => value.identityKey === identityKey
      );
      collision?.reject(new Error('Relocation reply relay ACK target source collided.'));
      return;
    }
    try {
      await this.requireAdmittedPeer(
        meshName,
        pending.ackTargetNodeRid,
        pending.expectedRequestSource,
        signal
      );
      if (!sameWireId(ack.relocation, pending.request.relocation)
        || !sameWireId(ack.operation, pending.request.operation)
        || ack.replyRouteId !== pending.request.replyRouteId
        || !sameCoordinator(ack.coordinator, pending.request.coordinator)
        || !sameRequestSource(ack.requestSource, pending.expectedRequestSource)) {
        throw new Error('Relocation reply relay ACK does not match its durable source fence.');
      }
      pending.resolve(ack.status);
    } catch (error) {
      pending.reject(error);
    }
  }

  private async requireAdmittedPeer(
    meshName: string,
    authenticatedRid: string,
    expected: ServiceWireRequestSourceFence | undefined,
    signal?: AbortSignal
  ): Promise<void> {
    const descriptor = (await this.options.liveDescriptors(meshName, signal)).find(
      value => String(value.rid) === authenticatedRid && value.state === 1
    );
    if (descriptor === undefined
      || expected !== undefined && (
        descriptor.ownerId !== expected.ownerId
        || descriptor.leaseGeneration !== expected.leaseGeneration
        || String(descriptor.rid) !== expected.nodeRid
        || descriptor.lifecycleGeneration !== expected.nodeGeneration
      )) {
      throw new Error('Relocation reply relay peer is not the exact admitted source fence.');
    }
  }

  private targetReplyRelayCoordinator(
    meshName: string,
    stage: LocalStage,
    authority: ZLinkAuthoritySnapshot
  ): ServiceWireRelocationCoordinatorFence {
    const status = this.requireMeshNode(meshName).status();
    const owner = this.options.currentOwner();
    const target = stage.offer.prepare.target;
    if (owner === undefined
      || owner.ownerId !== target.ownerId
      || owner.leaseGeneration !== target.ownerLeaseGeneration
      || String(status.routingId) !== target.nodeRid
      || status.lifecycleGeneration !== target.nodeGeneration) {
      throw new Error('Relocation reply relay target coordinator fence is stale.');
    }
    return {
      ownerId: owner.ownerId,
      leaseGeneration: owner.leaseGeneration,
      nodeRid: String(status.routingId),
      nodeGeneration: status.lifecycleGeneration,
      expectedAuthorityStoreVersion: authority.storeVersion.value
    };
  }

  private async relayTerminalReplies(
    meshName: string,
    stage: LocalStage,
    coordinator: ServiceWireRelocationCoordinatorFence,
    signal?: AbortSignal
  ): Promise<void> {
    for (const [participantIndex, participant] of stage.staging.envelope.participants.entries()) {
      const hidden = stage.staging.hidden.get(participant.key);
      if (hidden?.actor === undefined) continue;
      for (const [resultIndex, packet] of hidden.replayPackets.entries()) {
        if (!packet.returnResponse || packet.source === undefined) continue;
        const result = hidden.replayResults.at(resultIndex);
        if (result === undefined || result.index !== packet.index) {
          throw new Error('Relocation request result changed after target dispatch.');
        }
        const completion: RelocationTerminalCompletion = {
          index: packet.index,
          operationId: packet.messageFollowContext.operationId,
          source: packet.source,
          result
        };
        await this.sendReplyRelay(
          meshName,
          packet.source.nodeRid as RoutingId,
          maintenanceReplyRelay(
            stage,
            coordinator,
            BigInt(participantIndex + 1),
            completion
          ),
          handoffSourceFence(packet.source),
          signal
        );
      }
    }
  }

  private async exactSourceLeaseExpired(
    source: ServiceWireRequestSourceFence,
    signal?: AbortSignal
  ): Promise<boolean> {
    const lease = await this.requireLocationStore().readOwnerLease(source.ownerId, signal);
    return lease.kind === 'missing'
      || lease.token.ownerId !== source.ownerId
      || lease.token.leaseGeneration !== source.leaseGeneration
      || lease.leaseExpiresAt.getTime() <= lease.storeNow.getTime();
  }

  private async publishSessionRoutes(
    staging: ServiceObjectRelocationStaging<LocalHidden>
  ): Promise<void> {
    for (const hidden of staging.hidden.values()) {
      if (hidden.actor !== undefined) {
        await this.options.actorTransfer.publishRoutedActorOwnership(hidden.actor);
      }
    }
  }

  private async sendControl(
    meshName: string,
    targetNodeRid: RoutingId,
    request: ZLinkServiceRelocationControlRequest,
    signal?: AbortSignal,
    deadlineAtMs = Date.now() + 30_000
  ): Promise<ReturnType<typeof decodeServiceRelocationControlResponse>> {
    const node = this.requireMeshNode(meshName);
    const key = controlAckKey(request);
    if (this.pendingControls.has(key)) {
      throw new Error(`Relocation control ACK '${key}' is already pending.`);
    }
    return await new Promise<ZLinkServiceRelocationControlResponse>((resolve, reject) => {
      let pending!: PendingRelocationControl;
      const send = async (): Promise<void> => {
        if (signal?.aborted === true) {
          finish(() => reject(signal.reason));
          return;
        }
        if (Date.now() >= deadlineAtMs) {
          finish(() => reject(new Error(`Relocation control ACK '${key}' timed out.`)));
          return;
        }
        try {
          await node.sendToNode(targetNodeRid, encodeServiceRelocationControlRequest(request));
          if (this.pendingControls.get(key) === pending) {
            pending.timer = setTimeout(() => void send(), 250);
          }
        } catch (error) {
          pending.reject(error);
        }
      };
      const finish = (action: () => void) => {
        if (pending.timer !== undefined) clearTimeout(pending.timer);
        this.pendingControls.delete(key);
        action();
      };
      pending = {
        targetNodeRid: String(targetNodeRid), request,
        resolve: response => finish(() => resolve(response)),
        reject: error => finish(() => reject(error))
      };
      this.pendingControls.set(key, pending);
      void send();
    });
  }

  private acceptControlResponse(
    packet: ZLinkServiceRelocationControlRequest,
    sourceNodeRid: RoutingId | null
  ): boolean {
    const key = controlResponseKey(packet);
    if (key === undefined) return false;
    const pending = this.pendingControls.get(key);
    if (pending === undefined) return false;
    if (sourceNodeRid === null || String(sourceNodeRid) !== pending.targetNodeRid) {
      pending.reject(new Error('Relocation control ACK source node changed.'));
      return true;
    }
    try {
      const response = packet as ZLinkServiceRelocationControlResponse;
      validateControlResponse(pending.request, response);
      pending.resolve(response);
    } catch (error) {
      pending.reject(error);
    }
    return true;
  }

  private async selectTarget(
    meshName: string,
    localRid: string,
    targetApplicationVersion?: bigint,
    requirements: readonly RelocationTargetRequirement[] = [],
    signal?: AbortSignal
  ): Promise<ZLinkMeshNodeDescriptor> {
    const descriptors = await this.options.liveDescriptors(meshName, signal);
    const local = this.options.localDescriptor?.(meshName)
      ?? descriptors.find(descriptor => String(descriptor.rid) === localRid);
    const requiredVersion = targetApplicationVersion ?? local?.applicationVersion;
    const peers = this.requireMeshNode(meshName).peers();
    const candidates = descriptors
      .filter(descriptor =>
        String(descriptor.rid) !== localRid
        && descriptor.state === ZLinkFrameworkRuntimeState.Serving
        && descriptor.objectRole === ZLinkObjectRole.Server
        && descriptor.placementWeight > 0
        && (requiredVersion === undefined
          || descriptor.applicationVersion === requiredVersion)
        && (local?.maintenanceWave === undefined
          || descriptor.maintenanceWave !== local.maintenanceWave)
        && peers.some(peer => peer.routingId !== null
          && String(peer.routingId) === String(descriptor.rid)
          && peer.lifecycleGeneration === descriptor.lifecycleGeneration
          && peer.state === 3)
        && relocationTargetSupports(descriptor, requirements))
      .sort((left, right) => String(left.rid).localeCompare(String(right.rid)));
    const target = selectWeightedRelocationTarget(candidates);
    if (target === undefined) throw new Error(`RouteMesh '${meshName}' has no relocation target.`);
    return target;
  }

  private async readTargetParticipantAuthorities(
    envelope: ServiceRelocationEnvelope,
    signal?: AbortSignal
  ): Promise<ReadonlyMap<string, ZLinkAuthoritySnapshot>> {
    const authorities = new Map<string, ZLinkAuthoritySnapshot>();
    for (const participant of envelope.participants) {
      const current = await requireAuthority(
        this.requireLocationStore(),
        { value: participant.key } as ZLinkAuthorityKey,
        signal
      );
      if (current.objectGeneration !== participant.objectGeneration
        || current.authorityOwnerGeneration !== participant.authorityOwnerGeneration
        || current.allocation.objectKind !== participant.objectKind
        || current.allocation.stableType !== participant.stableType
        || current.allocation.state !== 'active') {
        throw new Error(`Relocation participant '${participant.key}' authority changed before reservation.`);
      }
      authorities.set(participant.key, current);
    }
    return authorities;
  }

  private async reserveTargetForPrepare(
    meshName: string,
    prepare: ServiceMaintenanceRelocationPrepare,
    envelope: ServiceRelocationEnvelope,
    signal?: AbortSignal
  ): Promise<TargetRelocationReservation> {
    const authorities = await this.readTargetParticipantAuthorities(envelope, signal);
    const publication = relocationPublication(prepare, envelope);
    const participants = envelope.participants.map(participant => {
      const expected = authorities.get(participant.key)!;
      return {
        key: { value: participant.key } as ZLinkAuthorityKey,
        expected,
        ownerTransition: 'newOwner' as const,
        authorityPayload: this.authorityPayloadForPublication(expected.payload, publication, {
          owner: {
            ownerId: prepare.target.ownerId,
            leaseGeneration: prepare.target.ownerLeaseGeneration
          },
          meshName,
          nodeRid: prepare.target.nodeRid,
          nodeGeneration: prepare.target.nodeGeneration,
          ...actorMembershipTarget(envelope, participant.key)
        }),
        membershipMutation: encodeMembershipMutation(envelope.memberships, participant.key)
      };
    });
    const plan: ServiceRelocationAggregatePlan = {
      envelope,
      participants,
      targetDescriptor: { meshName, rid: prepare.target.nodeRid as RoutingId },
      targetDescriptorLifecycleGeneration: prepare.target.nodeGeneration,
      capacity: participants.reduce((sum, participant) =>
        addCapacity(sum, participant.expected.allocation.capacity), { actors: 0, spots: 0 }),
      targetOwner: {
        ownerId: prepare.target.ownerId,
        leaseGeneration: prepare.target.ownerLeaseGeneration
      }
    };
    return {
      prepared: await new ServiceRelocationAggregateCommitter(this.requireLocationStore())
        .prepare(plan, signal)
    };
  }

  private authorityPayloadForPublication(
    payload: Uint8Array,
    publication: ServiceRelocationPublication,
    target?: {
      readonly owner: ZLinkLocationOwnerToken;
      readonly meshName: string;
      readonly nodeRid: string;
      readonly nodeGeneration: bigint;
      readonly actorSpotId?: string;
      readonly actorSpotGeneration?: bigint;
    }
  ): Uint8Array {
    const existing = this.codec.read(payload);
    if (existing === undefined) {
      const applicationPayload = target === undefined
        ? Buffer.from(payload)
        : rewriteAuthorityApplicationRoute(payload, target);
      return this.codec.publish(applicationPayload, publication);
    }
    if (existing.reference !== publication.reference
      || existing.checksumCrc32c !== publication.checksumCrc32c
      || existing.aggregateId !== publication.aggregateId
      || existing.aggregateGeneration !== publication.aggregateGeneration
      || existing.inventoryDigest !== publication.inventoryDigest
      || existing.targetOwnerId !== publication.targetOwnerId
      || existing.targetOwnerLeaseGeneration
        !== publication.targetOwnerLeaseGeneration) {
      throw new Error('Relocation authority contains another published root.');
    }
    return payload;
  }

  private async abortTargetReservation(
    reservation: TargetRelocationReservation,
    signal?: AbortSignal
  ): Promise<void> {
    await new ServiceRelocationAggregateCommitter(this.requireLocationStore())
      .abort(reservation.prepared, signal);
  }

  private async abortTargetStage(stage: LocalStage, signal?: AbortSignal): Promise<void> {
    if (stage.fallback !== undefined) clearTimeout(stage.fallback);
    const failures: unknown[] = [];
    try {
      await this.abortTargetReservation(stage.offer.reservation, signal);
    } catch (error) {
      failures.push(error);
    }
    try {
      await stage.owner.abort(stage.staging);
    } catch (error) {
      failures.push(error);
    }
    stage.phase = 'failed';
    if (failures.length === 1) throw failures[0];
    if (failures.length > 1) {
      throw new AggregateError(failures, 'Relocation target rollback was incomplete.');
    }
  }

  private async commitTargetReservation(
    stage: LocalStage,
    reservation: TargetRelocationReservation,
    signal?: AbortSignal
  ): Promise<ZLinkAuthoritySnapshot> {
    return await this.commitTargetAggregate(stage, reservation.prepared, signal);
  }

  private async commitTargetAggregate(
    stage: LocalStage,
    prepared: ServicePreparedRelocationAggregate,
    signal?: AbortSignal
  ): Promise<ZLinkAuthoritySnapshot> {
    let firstError: unknown;
    for (;;) {
      signal?.throwIfAborted();
      let result: Awaited<ReturnType<ZLinkLocationStore['commitAggregate']>> | undefined;
      try {
        result = await this.requireLocationStore().commitAggregate(prepared.fence, signal);
      } catch (error) {
        firstError ??= error;
      }
      if (result?.kind === 'stale' || result?.kind === 'generationExhausted') {
        throw new Error(
          `location_update_failed: relocation aggregate commit returned ${result.kind}.`,
          { cause: firstError }
        );
      }
      const observed = await this.readAggregateForCommitRetry(prepared, signal);
      if (observed.kind === 'committed') {
        const primary = observed.authorities.get(stage.staging.primaryAuthorityKey.value);
        if (primary === undefined) {
          throw new Error('Committed relocation aggregate omitted its primary authority.');
        }
        return primary;
      }
      if (observed.kind === 'stale') {
        throw new Error('location_update_failed: relocation aggregate source authority became stale.', {
          cause: firstError
        });
      }
      await waitForLocationRetry(stage.offer.restoreDeadlineAtMs, signal, firstError);
    }
  }

  private async readAggregateForCommitRetry(
    prepared: ServicePreparedRelocationAggregate,
    signal?: AbortSignal
  ): Promise<
    | { readonly kind: 'committed'; readonly authorities: ReadonlyMap<string, ZLinkAuthoritySnapshot> }
    | { readonly kind: 'source' | 'unknown' | 'stale' }
  > {
    const authorities = new Map<string, ZLinkAuthoritySnapshot>();
    let source = 0;
    let committed = 0;
    try {
      for (const participant of prepared.plan.participants) {
        const current = await this.requireLocationStore().readAuthority(participant.key, signal);
        if (current.kind !== 'snapshot') return { kind: 'stale' };
        if (isExactCommittedTarget(
          current,
          participant.expected,
          {
            nodeRid: String(prepared.plan.targetDescriptor.rid),
            nodeGeneration: prepared.plan.targetDescriptorLifecycleGeneration,
            ownerId: prepared.plan.targetOwner.ownerId,
            ownerLeaseGeneration: prepared.plan.targetOwner.leaseGeneration
          },
          participant.authorityPayload
        )) {
          committed += 1;
          authorities.set(participant.key.value, current);
        } else if (isExactSourceAuthority(current, participant.expected)) {
          source += 1;
        } else {
          return { kind: 'stale' };
        }
      }
    } catch {
      return { kind: 'unknown' };
    }
    if (committed === prepared.plan.participants.length) {
      return { kind: 'committed', authorities };
    }
    return source === prepared.plan.participants.length
      ? { kind: 'source' }
      : { kind: 'stale' };
  }

  private async materializeTargetOffer(
    meshName: string,
    stagingId: string,
    offer: TargetRelocationOffer,
    signal?: AbortSignal
  ): Promise<LocalStage> {
    const target = new LocalTargetPort(this.options, meshName, offer.envelope);
    const owner = new ServiceRelocationObjectRestoreOwner(
      target,
      value => ({ value } as ZLinkAuthorityKey)
    );
    const staging = await owner.prepare(offer.envelope, signal);
    if (staging.id !== `${offer.envelope.aggregateId}:${offer.envelope.aggregateGeneration}`) {
      throw new Error('Relocation materialization returned a different staging identity.');
    }
    const stage: LocalStage = {
      offer,
      owner,
      staging,
      phase: 'ready',
      lane: Promise.resolve(),
      cutoverReceived: false
    };
    this.targetStages.set(stagingId, stage);
    return stage;
  }

  private spotKind(
    meshName: string,
    activation: ZLinkSpotActivation
  ): 'user_spot' | 'instance_spot' | undefined {
    const node = this.options.registration.spotNodes.get(meshName);
    const registrations = activation.domain.kind === 'user'
      ? node?.spotFactoryRegistrations
      : node?.instanceSpotFactoryRegistrations;
    return Object.values(registrations ?? {})
      .some(value => value.implementation === activation.spotType)
      ? activation.domain.kind === 'user' ? 'user_spot' : 'instance_spot'
      : undefined;
  }

  private spotRegistration(
    meshName: string,
    kind: 'user_spot' | 'instance_spot',
    stableType: string
  ) {
    const node = this.options.registration.spotNodes.get(meshName);
    const value = kind === 'user_spot'
      ? node?.spotFactoryRegistrations?.[stableType]
      : node?.instanceSpotFactoryRegistrations?.[stableType];
    if (value === undefined) throw new Error(`Relocation Spot type '${stableType}' is not registered.`);
    return value;
  }

  private actorRegistration(meshName: string, stableType: string) {
    const value = this.options.registration.spotNodes.get(meshName)
      ?.actorFactoryRegistrations?.[stableType];
    if (value === undefined) throw new Error(`Relocation Actor type '${stableType}' is not registered.`);
    return value;
  }

  private async captureApplication<T>(
    policy: { readonly kind: 'disabled' | 'recreate' | 'snapshot'; readonly adapterType?: Type },
    value: T,
    signal?: AbortSignal
  ): Promise<Uint8Array> {
    if (policy.kind === 'disabled') {
      throw new Error(`Relocation is disabled for '${(value as { constructor?: { name?: string } }).constructor?.name ?? 'object'}'.`);
    }
    if (policy.kind === 'recreate') return Buffer.alloc(0);
    const adapter = await createProviderInstance(policy.adapterType!, this.options.providerResolver) as
      ZLinkActorRelocationAdapter<ZLinkActor> | ZLinkSpotRelocationAdapter<ZLinkSpot | ZLinkInstanceSpot>;
    const captured = Buffer.from(
      await adapter.capture(value as never, signal ?? new AbortController().signal)
    );
    if (captured.byteLength > RELOCATION_STRIPE_BYTES) {
      throw new ZLinkRelocationStateIncompatibleError(
        'Relocation application state exceeds the 64 MiB participant limit.'
      );
    }
    return captured;
  }

  private recordRelocationInterruption(
    meshName: string,
    unitKind: 'actor' | 'user_spot' | 'instance_spot',
    executionMode: 'spot_wide' | 'per_actor' | undefined,
    startedAt: number,
    outcome: string
  ): void {
    const seconds = Math.max(0, performance.now() - startedAt) / 1000;
    const attributes = {
      unit_kind: unitKind,
      ...(executionMode === undefined ? {} : { execution_mode: executionMode })
    };
    this.options.metrics?.duration(
      'zlink.relocation.interruption',
      seconds,
      attributes
    );
    this.options.metrics?.count('zlink.relocation.completed', 1, {
      mesh_name: meshName,
      object_kind: unitKind,
      policy: unitKind === 'user_spot' && executionMode === 'per_actor'
        ? 'recreate'
        : 'configured',
      outcome
    });
    if (seconds <= 1) return;
    console.warn(
      `[zlink.runtime.relocation.changed] ${unitKind} admission interruption ` +
      `exceeded 1 second (${seconds.toFixed(3)}s).`
    );
    void this.options.runtimeEventPublisher?.publish({
      sourceName: 'zlink.runtime.relocation',
      timestamp: new Date(),
      identifier: 'zlink.runtime.relocation.changed',
      meshName,
      unitKind,
      executionMode,
      interruptionTargetExceeded: true,
      durationSeconds: seconds
    });
  }

  private sealSpotMessageFollow(
    meshName: string,
    authority: ZLinkAuthoritySnapshot,
    activation: ZLinkSpotActivation
  ): ServiceSpotMessageFollowSeal | undefined {
    const node = this.requireMeshNode(meshName);
    const seal = node.sealSpotMessageFollowIngress;
    const abort = node.abortSpotMessageFollowIngress;
    const commit = node.commitSpotMessageFollowIngress;
    if (seal === undefined && abort === undefined && commit === undefined) return undefined;
    if (seal === undefined || abort === undefined || commit === undefined) {
      throw new Error('Spot Message Follow backend support is incomplete.');
    }
    const status = node.status();
    const result = seal.call(node, {
      spot: {
        spotId: String(activation.spotId),
        generation: authority.objectGeneration
      },
      targetNodeRid: String(status.routingId),
      targetNodeGeneration: status.lifecycleGeneration,
      authorityOwnerGeneration: authority.authorityOwnerGeneration,
      ownerLeaseGeneration: authority.ownerLeaseGeneration,
      storeVersion: authority.storeVersion.value
    });
    if (result === undefined) {
      throw new Error(
        `Spot '${String(activation.spotId)}' ingress could not be sealed for relocation.`
      );
    }
    return result;
  }

  private async commitSpotMessageFollow(
    meshName: string,
    seal: ServiceSpotMessageFollowSeal,
    authorityKey: ZLinkAuthorityKey,
    activation: ZLinkSpotActivation,
    target: ZLinkMeshNodeDescriptor
  ): Promise<void> {
    const node = this.requireMeshNode(meshName);
    const commit = node.commitSpotMessageFollowIngress;
    if (commit === undefined) {
      throw new Error('Spot Message Follow backend support is incomplete.');
    }
    const committed = await requireAuthority(
      this.requireLocationStore(),
      authorityKey
    );
    const accepted = await commit.call(node, seal, {
      spot: {
        spotId: String(activation.spotId),
        generation: committed.objectGeneration
      },
      targetNodeRid: String(target.rid),
      targetNodeGeneration: target.lifecycleGeneration,
      authorityOwnerGeneration: committed.authorityOwnerGeneration,
      ownerLeaseGeneration: committed.ownerLeaseGeneration,
      storeVersion: committed.storeVersion.value
    }, this.relocationLimits().messageFollowDurationMs);
    if (!accepted) {
      throw new Error(
        `Spot '${String(activation.spotId)}' Message Follow route became stale.`
      );
    }
  }

  private requireLocationStore() {
    const value = this.options.locationStore();
    if (value === undefined) throw new Error('Host relocation requires a Location Store.');
    return value;
  }

  private requireRelocationStore(): ZLinkRelocationStore {
    const value = this.options.relocationStore();
    if (value === undefined) throw new Error('Host relocation requires a Relocation Store.');
    return value;
  }

  private requireSpotManager(): DefaultZLinkSpotManager {
    const value = this.options.spotManager();
    if (value === undefined) throw new Error('Host relocation requires the Spot manager.');
    return value;
  }

  private requireActorManager(): DefaultZLinkActorManager {
    const value = this.options.actorManager();
    if (value === undefined) throw new Error('Host relocation requires the Actor manager.');
    return value;
  }

  private relocationLimits() {
    const configured = (
      this.options.registration as unknown as {
        readonly locations?: {
          readonly options?: Partial<typeof zlinkRuntimeDefaultLocationOptions>;
        };
      }
    ).locations?.options;
    return {
      ...zlinkRuntimeDefaultLocationOptions,
      ...(configured ?? {})
    };
  }

  private requireMeshNode(meshName: string): ZLinkBackendMeshNode {
    const value = this.options.meshNode(meshName);
    if (value === undefined) throw new Error(`RouteMesh '${meshName}' is not started.`);
    return value;
  }
}

class LocalTargetPort implements ServiceRelocationTargetObjectPort<LocalHidden> {
  constructor(
    private readonly options: ZLinkHostRelocationOptions,
    private readonly meshName: string,
    private readonly envelope: ServiceRelocationEnvelope
  ) {}

  async createHidden(
    participant: ServiceRelocationParticipant,
    signal?: AbortSignal
  ): Promise<LocalHidden> {
    const identity = decodeAuthorityKey({ value: participant.key } as ZLinkAuthorityKey).globalId;
    if (participant.objectKind === 'actor') {
      const membership = this.envelope.memberships.find(value => value.actorKey === participant.key);
      if (membership === undefined) throw new Error(`Actor '${identity}' relocation membership is missing.`);
      const spotIdentity = decodeAuthorityKey({ value: membership.spotKey } as ZLinkAuthorityKey).globalId;
      const localDescriptor = this.options.localDescriptor?.(this.meshName);
      const nativeEntrySpot = localDescriptor?.entrySpotId === spotIdentity;
      const actor = await this.requireActorManager().prepareRelocationActor(
        identity,
        participant.stableType,
        participant.objectGeneration,
        participant.authorityOwnerGeneration + 1n,
        (nativeEntrySpot ? localDescriptor.rid : spotIdentity) as RoutingId,
        nativeEntrySpot ? localDescriptor.lifecycleGeneration : membership.spotObjectGeneration,
        membership.membershipEpoch,
        signal
      );
      return {
        authorityKey: participant.key,
        participant,
        actor,
        initialized: false,
        restoredTimers: [],
        replayResults: [],
        replayPackets: []
      };
    }
    const registration = this.registration(participant.objectKind, participant.stableType);
    const activation = await this.requireSpotManager().prepareRelocationSpot(
      this.meshName,
      participant.objectKind,
      participant.stableType,
      registration.implementation as Type<ZLinkSpot | ZLinkInstanceSpot>,
      identity as RoutingId,
      participant.objectGeneration,
      participant.authorityOwnerGeneration + 1n,
      signal
    );
    return {
      authorityKey: participant.key,
      participant,
      activation,
      initialized: false,
      restoredTimers: [],
      replayResults: [],
      replayPackets: []
    };
  }

  async restoreApplicationState(
    hidden: LocalHidden,
    payload: Uint8Array,
    signal?: AbortSignal
  ): Promise<void> {
    const registration = this.registration(hidden.participant.objectKind, hidden.participant.stableType);
    if (registration.relocation.kind === 'recreate') {
      if (payload.byteLength !== 0) throw new Error('Recreate relocation contains snapshot state.');
      return;
    }
    if (registration.relocation.kind !== 'snapshot') {
      throw new Error(`Relocation is disabled for '${hidden.participant.stableType}'.`);
    }
    const adapter = await createProviderInstance(
      registration.relocation.adapterType,
      this.options.providerResolver
    ) as ZLinkActorRelocationAdapter<ZLinkActor> | ZLinkSpotRelocationAdapter<ZLinkSpot | ZLinkInstanceSpot>;
    await adapter.restore(
      (hidden.actor ?? hidden.activation?.spot) as never,
      payload,
      signal ?? new AbortController().signal
    );
  }

  async restoreMemberships(
    hidden: ReadonlyMap<string, LocalHidden>,
    memberships: readonly ServiceRelocationMembership[]
  ): Promise<void> {
    for (const membership of memberships) {
      const actor = hidden.get(membership.actorKey)?.actor;
      const spotIdentity = decodeAuthorityKey(
        { value: membership.spotKey } as ZLinkAuthorityKey
      ).globalId;
      const spot = hidden.get(membership.spotKey)?.activation
        ?? this.requireSpotManager().resolveRelocationActivation(
          this.meshName,
          spotIdentity as RoutingId
        );
      if (actor === undefined) throw new Error('Relocation membership Actor staging is missing.');
      const state = this.requireActorManager().getState(actor.context.actorId)!;
      const localDescriptor = this.options.localDescriptor?.(this.meshName);
      const isEntryMembership = localDescriptor?.entrySpotId === spotIdentity;
      if (isEntryMembership) {
        // Entry Spot identity is published as a Location membership key, but
        // it is not a User Spot activation in the Actor runtime. The native
        // restore already received the target MeshNode RID as its Entry
        // membership; clearing the framework Spot state keeps dispatch on the
        // local Entry Spot path after the target commit.
        state.clearJoinedSpot();
      } else {
        state.setJoinedSpot(
          spotIdentity as RoutingId,
          spot?.spot,
          membership.membershipEpoch
        );
      }
      spot?.commitActorJoin(actor);
    }
  }

  async publish(
    hidden: LocalHidden,
    authority: ZLinkAuthoritySnapshot
  ): Promise<void> {
    if (hidden.actor !== undefined) {
      const store = this.requireLocationStore();
      const current = await requireAuthority(
        store,
        { value: hidden.authorityKey } as ZLinkAuthorityKey
      );
      this.requireActorManager().adoptCreatedAuthority(
        hidden.actor.context.actorId,
        current.authorityOwnerGeneration,
        current.ownerLeaseGeneration
      );
      hidden.targetAuthorityOwnerGeneration = current.authorityOwnerGeneration;
    } else if (hidden.activation !== undefined) {
      const registration = this.registration(
        hidden.participant.objectKind,
        hidden.participant.stableType
      );
      const applicationSignaled =
        hidden.participant.objectKind === 'user_spot'
        && registration.options?.relocationReadiness
          === ZLinkSpotRelocationReadinessMode.ApplicationSignaled;
      await this.requireSpotManager().publishRelocationSpot(
        hidden.activation,
        applicationSignaled
          ? () => hidden.activation!.notifyRelocatedBoundary()
          : undefined
      );
      this.options.meshNode(this.meshName)?.rememberSpotRoute?.({
        spot: {
          spotId: String(hidden.activation.spotId),
          generation: authority.objectGeneration
        },
        targetNodeRid: String(authority.allocation.descriptor.rid),
        targetNodeGeneration: authority.allocation.descriptorLifecycleGeneration,
        authorityOwnerGeneration: authority.authorityOwnerGeneration,
        ownerLeaseGeneration: authority.ownerLeaseGeneration,
        storeVersion: authority.storeVersion.value
      });
      if (hidden.participant.objectKind === 'instance_spot') {
        this.options.trackInstanceSpot?.({
          meshName: this.meshName,
          spotId: hidden.activation.spotId,
          stableType: hidden.participant.stableType,
          nodeRid: authority.allocation.descriptor.rid,
          nodeGeneration: authority.allocation.descriptorLifecycleGeneration,
          objectGeneration: authority.objectGeneration,
          authorityOwnerGeneration: authority.authorityOwnerGeneration,
          ownerId: authority.ownerId,
          ownerLeaseGeneration: authority.ownerLeaseGeneration,
          storeVersion: authority.storeVersion.value,
          deactivate: () => this.requireSpotManager().abortRelocationSpot(hidden.activation!)
        });
      }
    }
    hidden.initialized = true;
  }

  async restoreBoundSession(hidden: LocalHidden, payload: Uint8Array): Promise<void> {
    if (hidden.actor === undefined || payload.byteLength === 0) return;
    const target = decodeActorSession(payload);
    const state = this.requireActorManager().getState(hidden.actor.context.actorId)!;
    state.setRemoteBoundSessionTarget(target);
    state.setBoundSessionTransferTarget(target);
    if (target.bindingGeneration !== undefined) {
      state.setBoundSessionBindingGeneration(target.bindingGeneration);
    }
  }

  async replayQueuedMessage(
    hidden: LocalHidden,
    message: ServiceRelocationQueuedMessage
  ): Promise<void> {
    if (hidden.actor === undefined) {
      throw new Error('Only Actor relocation participants can contain packet backlog.');
    }
    const packet = decodeQueuedHandoffPacket(message);
    const state = this.requireActorManager().getState(hidden.actor.context.actorId);
    if (state === undefined) throw new Error('Relocated Actor state is not staged.');
    hidden.replayPackets.push(packet);
  }

  async openAdmission(hidden: LocalHidden): Promise<void> {
    if (hidden.actor === undefined) return;
    this.requireActorManager().publishRelocationActor(hidden.actor.context.actorId);
    if (hidden.replayPackets.length === 0) return;
    const state = this.requireActorManager().getState(hidden.actor.context.actorId);
    if (state === undefined) throw new Error('Relocated Actor state is not staged.');
    const results = await replayActorHandoffBacklog(
      hidden.replayPackets,
      (parts, returnResponse, remoteBoundSessionTarget, fallbackActorRef) => {
        if (state.spotId === undefined) {
          const runtime = this.options.spotNodeRuntime();
          if (runtime === undefined) {
            throw new Error('Relocated Entry Spot Actor dispatch is unavailable.');
          }
          return runtime.dispatchEntryActorPacket(
            hidden.actor!.context.actorId,
            parts,
            returnResponse,
            remoteBoundSessionTarget,
            fallbackActorRef
          );
        }
        return this.requireSpotManager().dispatchRoutedActorPacket(
          state.spotId,
          hidden.actor!.context.actorId,
          parts,
          returnResponse,
          remoteBoundSessionTarget,
          fallbackActorRef
        );
      }
    );
    hidden.replayResults.push(...results);
  }

  async restoreTimer(hidden: LocalHidden, timer: ServiceRelocationTimer): Promise<void> {
    if (hidden.activation === undefined) throw new Error('Actor relocation cannot restore a Spot timer.');
    hidden.restoredTimers.push(timer);
    if (hidden.restoredTimers.length !== hidden.participant.timers.length) return;
    hidden.activation.timers.restoreRelocation(hidden.restoredTimers.map(value => ({
      name: value.timerId,
      periodMs: value.intervalMs,
      overrunPolicy: value.overrunPolicy as never,
      maxCatchUpTicks: value.maxCatchUpTicks,
      stopOnUnhandledException: value.stopOnUnhandledException,
      startedAtUnixMs: value.startedAtUnixMs,
      deliveryIndex: value.deliveryIndex,
      lastScheduledIndex: value.lastScheduledIndex,
      pendingTicks: value.pendingTicks
    })));
  }

  async normalize(_hidden: LocalHidden): Promise<void> {}

  async abort(hidden: LocalHidden): Promise<void> {
    if (hidden.actor !== undefined) {
      await this.requireActorManager().abortRelocationActor(hidden.actor.context.actorId);
    } else if (hidden.activation !== undefined) {
      await this.requireSpotManager().abortRelocationSpot(hidden.activation);
    }
  }

  private registration(kind: ServiceRelocationParticipant['objectKind'], stableType: string): any {
    const node = this.options.registration.spotNodes.get(this.meshName);
    const value = kind === 'actor'
      ? node?.actorFactoryRegistrations?.[stableType]
      : kind === 'user_spot'
        ? node?.spotFactoryRegistrations?.[stableType]
        : node?.instanceSpotFactoryRegistrations?.[stableType];
    if (value === undefined) throw new Error(`Relocation type '${stableType}' is not registered.`);
    return value;
  }

  private requireLocationStore() {
    const store = this.options.locationStore();
    if (store === undefined) throw new Error('Relocation target Location Store is unavailable.');
    return store;
  }

  private requireSpotManager(): DefaultZLinkSpotManager {
    const manager = this.options.spotManager();
    if (manager === undefined) throw new Error('Relocation target Spot manager is unavailable.');
    return manager;
  }

  private requireActorManager(): DefaultZLinkActorManager {
    const manager = this.options.actorManager();
    if (manager === undefined) throw new Error('Relocation target Actor manager is unavailable.');
    return manager;
  }
}

function relocationStorePort(store: ZLinkRelocationStore): ServiceRelocationStorePort {
  return {
    async put(payload, retentionMs, signal) {
      if (payload.byteLength > RELOCATION_STRIPE_BYTES) {
        const stripes = splitRelocationPayload(payload);
        const stored: Array<{
          readonly reference: string;
          readonly checksumCrc32c: number;
          readonly byteLength: number;
        }> = [];
        try {
          const receipts = await mapConcurrentOrdered(
            stripes,
            RELOCATION_STRIPE_CONCURRENCY,
            async stripe => {
              const result = await putNewRelocationBlob(
                store,
                stripe,
                retentionMs,
                signal
              );
              return {
                reference: result.reference.value,
                checksumCrc32c: crc32c(stripe),
                byteLength: stripe.byteLength,
                expiresAt: result.expiresAt,
                storeNow: result.storeNow
              };
            },
            signal
          );
          stored.push(...receipts);
          const manifest = Buffer.from(JSON.stringify({
            kind: RELOCATION_STRIPE_MANIFEST_KIND,
            byteLength: payload.byteLength,
            checksumCrc32c: crc32c(payload),
            stripes: receipts.map(({ reference, checksumCrc32c, byteLength }) => ({
              reference,
              checksumCrc32c,
              byteLength
            }))
          }), 'utf8');
          const root = await putNewRelocationBlob(store, manifest, retentionMs, signal);
          return {
            reference: root.reference.value,
            checksumCrc32c: crc32c(payload),
            expiresAtMs: Math.min(
              root.expiresAt.getTime(),
              ...receipts.map(value => value.expiresAt.getTime())
            ),
            storeNowMs: Math.max(
              root.storeNow.getTime(),
              ...receipts.map(value => value.storeNow.getTime())
            )
          };
        } catch (error) {
          await Promise.all(stored.map(value =>
            store.delete(relocationBlobReference(value.reference), signal)
              .catch(() => undefined)));
          throw error;
        }
      }
      const result = await putNewRelocationBlob(
        store,
        payload,
        retentionMs,
        signal
      );
      return {
        reference: result.reference.value,
        checksumCrc32c: crc32c(payload),
        expiresAtMs: result.expiresAt.getTime(),
        storeNowMs: result.storeNow.getTime()
      };
    },
    async get(reference, signal) {
      const result = await readRelocationPayload(store, reference, signal);
      return result.kind === 'missing'
        ? result
        : { kind: 'found', payload: result.payload };
    },
    async delete(reference, signal) {
      const read = await store.read(relocationBlobReference(reference), signal);
      const manifest = read.kind === 'found'
        ? decodeRelocationStripeManifest(read.bytes)
        : undefined;
      await store.delete(relocationBlobReference(reference), signal);
      if (manifest !== undefined) {
        await mapConcurrentOrdered(
          manifest.stripes,
          RELOCATION_STRIPE_CONCURRENCY,
          async stripe => {
            await store.delete(relocationBlobReference(stripe.reference), signal);
          },
          signal
        );
      }
      return 'deleted';
    }
  };
}

async function readRelocationPayload(
  store: ZLinkRelocationStore,
  reference: string,
  signal?: AbortSignal
): Promise<
  | { readonly kind: 'missing' }
  | {
      readonly kind: 'found';
      readonly payload: Buffer;
      readonly expiresAtMs: number;
      readonly storeNowMs: number;
    }
> {
  const result = await store.read(relocationBlobReference(reference), signal);
  if (result.kind !== 'found') return { kind: 'missing' };
  const manifest = decodeRelocationStripeManifest(result.bytes);
  if (manifest === undefined) {
    return {
      kind: 'found',
      payload: Buffer.from(result.bytes),
      expiresAtMs: result.expiresAt.getTime(),
      storeNowMs: result.storeNow.getTime()
    };
  }
  const stripes = await mapConcurrentOrdered(
    manifest.stripes,
    RELOCATION_STRIPE_CONCURRENCY,
    async stripe => {
      const read = await store.read(relocationBlobReference(stripe.reference), signal);
      if (read.kind !== 'found'
        || read.bytes.byteLength !== stripe.byteLength
        || crc32c(read.bytes) !== stripe.checksumCrc32c) {
        throw new Error(`Relocation stripe '${stripe.reference}' is missing or corrupt.`);
      }
      return {
        payload: Buffer.from(read.bytes),
        expiresAtMs: read.expiresAt.getTime(),
        storeNowMs: read.storeNow.getTime()
      };
    },
    signal
  );
  const payload = Buffer.concat(stripes.map(value => value.payload));
  if (payload.byteLength !== manifest.byteLength
    || crc32c(payload) !== manifest.checksumCrc32c) {
    throw new Error('Relocation striped payload failed root integrity validation.');
  }
  return {
    kind: 'found',
    payload,
    expiresAtMs: Math.min(result.expiresAt.getTime(), ...stripes.map(value => value.expiresAtMs)),
    storeNowMs: Math.max(result.storeNow.getTime(), ...stripes.map(value => value.storeNowMs))
  };
}

interface RelocationStripeManifest {
  readonly byteLength: number;
  readonly checksumCrc32c: number;
  readonly stripes: readonly {
    readonly reference: string;
    readonly checksumCrc32c: number;
    readonly byteLength: number;
  }[];
}

function splitRelocationPayload(payload: Uint8Array): readonly Buffer[] {
  const count = Math.ceil(payload.byteLength / RELOCATION_STRIPE_BYTES);
  if (count > RELOCATION_STRIPE_LIMIT) {
    throw new Error(`Relocation payload exceeds ${RELOCATION_STRIPE_LIMIT} stripes.`);
  }
  const source = Buffer.from(payload);
  return Array.from({ length: count }, (_, index) =>
    source.subarray(
      index * RELOCATION_STRIPE_BYTES,
      Math.min(source.byteLength, (index + 1) * RELOCATION_STRIPE_BYTES)
    ));
}

function decodeRelocationStripeManifest(
  payload: Uint8Array
): RelocationStripeManifest | undefined {
  if (payload.byteLength > 1024 * 1024) return undefined;
  let parsed: unknown;
  try {
    parsed = JSON.parse(Buffer.from(payload).toString('utf8'));
  } catch {
    return undefined;
  }
  if (!isRecord(parsed) || parsed.kind !== RELOCATION_STRIPE_MANIFEST_KIND
    || !Number.isSafeInteger(parsed.byteLength) || (parsed.byteLength as number) <= 0
    || !Number.isSafeInteger(parsed.checksumCrc32c)
    || !Array.isArray(parsed.stripes)
    || parsed.stripes.length < 2
    || parsed.stripes.length > RELOCATION_STRIPE_LIMIT) {
    return undefined;
  }
  const stripes = parsed.stripes.map((value, index) => {
    if (!isRecord(value)
      || typeof value.reference !== 'string' || value.reference.length === 0
      || !Number.isSafeInteger(value.checksumCrc32c)
      || !Number.isSafeInteger(value.byteLength)
      || (value.byteLength as number) <= 0
      || (value.byteLength as number) > RELOCATION_STRIPE_BYTES) {
      throw new Error(`Relocation stripe manifest entry ${index} is invalid.`);
    }
    return {
      reference: value.reference,
      checksumCrc32c: value.checksumCrc32c as number,
      byteLength: value.byteLength as number
    };
  });
  return {
    byteLength: parsed.byteLength as number,
    checksumCrc32c: parsed.checksumCrc32c as number,
    stripes
  };
}

async function requireAuthority(
  store: Pick<ZLinkLocationStore, 'readAuthority'>,
  key: ZLinkAuthorityKey,
  signal?: AbortSignal
): Promise<ZLinkAuthoritySnapshot> {
  const current = await store.readAuthority(key, signal);
  if (current.kind !== 'snapshot') throw new Error(`Authority '${key.value}' is missing.`);
  return current;
}

function rewriteAuthorityApplicationRoute(
  payload: Uint8Array,
  target: {
    readonly owner: ZLinkLocationOwnerToken;
    readonly meshName: string;
    readonly nodeRid: string;
    readonly nodeGeneration: bigint;
    readonly actorSpotId?: string;
    readonly actorSpotGeneration?: bigint;
  }
): Buffer {
  const applicationPayload = serviceRelocationAuthorityApplicationPayload(payload);
  const actor = decodeActorAuthorityIdentity(applicationPayload);
  if (actor !== undefined) {
    return rewriteActorAuthorityRoute(
      applicationPayload,
      {
        actorId: actor.actor.actorId,
        objectGeneration: actor.actor.objectGeneration,
        meshName: target.meshName,
        nodeRid: target.nodeRid as RoutingId
      },
      target.actorSpotId ?? actor.spotId ?? target.nodeRid,
      target.actorSpotGeneration ?? actor.spotGeneration ?? target.nodeGeneration,
      target.nodeGeneration,
      target.owner
    );
  }
  return rewriteServiceAuthorityRoute(applicationPayload, target.owner, target)
    ?? applicationPayload;
}

function actorMembershipTarget(
  envelope: ServiceRelocationEnvelope,
  actorKey: string
): { readonly actorSpotId: string; readonly actorSpotGeneration: bigint } | undefined {
  const membership = envelope.memberships.find(value => value.actorKey === actorKey);
  if (membership === undefined) return undefined;
  return {
    actorSpotId: decodeAuthorityKey({ value: membership.spotKey } as ZLinkAuthorityKey).globalId,
    actorSpotGeneration: membership.spotObjectGeneration
  };
}

function primaryKey(envelope: ServiceRelocationEnvelope): string {
  return envelope.participants.find(value => value.objectKind !== 'actor')?.key
    ?? envelope.participants[0]!.key;
}

function relocationTargetRequirements(
  values: readonly RelocationTargetRequirement[]
): readonly RelocationTargetRequirement[] {
  const grouped = new Map<string, RelocationTargetRequirement>();
  for (const value of values) {
    const key = `${value.objectKind}\0${value.stableType}\0${value.policy}`;
    const existing = grouped.get(key);
    grouped.set(key, existing === undefined
      ? value
      : { ...existing, count: existing.count + value.count });
  }
  return [...grouped.values()];
}

function relocationTargetSupports(
  descriptor: ZLinkMeshNodeDescriptor,
  requirements: readonly RelocationTargetRequirement[]
): boolean {
  if (descriptor.activationConcurrency.limit !== 0
      && descriptor.activationConcurrency.active >= descriptor.activationConcurrency.limit) {
    return false;
  }
  const actorCount = requirements
    .filter(value => value.objectKind === 'actor')
    .reduce((sum, value) => sum + value.count, 0);
  const spotCount = requirements
    .filter(value => value.objectKind !== 'actor')
    .reduce((sum, value) => sum + value.count, 0);
  if ((descriptor.populationCapacity.actors.limit !== 0
      && descriptor.populationCapacity.actors.active
      + descriptor.populationCapacity.actors.reserved
      + actorCount > descriptor.populationCapacity.actors.limit
    ) || (descriptor.populationCapacity.spots.limit !== 0
      && descriptor.populationCapacity.spots.active
      + descriptor.populationCapacity.spots.reserved
      + spotCount > descriptor.populationCapacity.spots.limit)) {
    return false;
  }
  for (const requirement of requirements) {
    if (!descriptor.objectCapabilities.some(capability =>
      capability.objectKind === requirement.objectKind
      && capability.stableType === requirement.stableType
      && capability.policy === requirement.policy)) {
      return false;
    }
    if (requirement.objectKind !== 'actor') {
      const capacity = descriptor.populationCapacity.spotTypes.find(value =>
        value.objectKind === requirement.objectKind
        && value.stableType === requirement.stableType);
      if (capacity === undefined
        || (capacity.limit !== 0
          && capacity.active + capacity.reserved + requirement.count > capacity.limit)) {
        return false;
      }
    }
  }
  return true;
}

function selectWeightedRelocationTarget(
  candidates: readonly ZLinkMeshNodeDescriptor[]
): ZLinkMeshNodeDescriptor | undefined {
  const total = candidates.reduce((sum, value) => sum + value.placementWeight, 0);
  if (total <= 0) return undefined;
  let selected = Math.random() * total;
  for (const candidate of candidates) {
    selected -= candidate.placementWeight;
    if (selected < 0) return candidate;
  }
  return candidates.at(-1);
}

interface RelocationTerminalCompletion {
  readonly index: number;
  readonly operationId: string;
  readonly source: NonNullable<ZLinkActorHandoffPacket['source']>;
  readonly result: ZLinkActorHandoffResult;
}

type RelocationTerminalDelivery =
  | 'terminalReceived'
  | 'alreadyTerminal'
  | 'sourceLeaseExpired';

function decodeQueuedHandoffPacket(message: ServiceRelocationQueuedMessage): ZLinkActorHandoffPacket {
  const value = JSON.parse(Buffer.from(message.payload).toString('utf8')) as Record<string, unknown>;
  const index = value.index;
  if (!Number.isSafeInteger(index) || index as number < 0) {
    throw new TypeError('Relocation Actor packet index is invalid.');
  }
  const decoded = decodeHandoffBacklog([{ ...value, index: 0 }])[0]!;
  return { ...decoded, index: index as number };
}

function relocationWireId(value: string): { readonly high: bigint; readonly low: bigint } {
  const hex = value.replaceAll('-', '');
  if (!/^[0-9a-fA-F]{32}$/.test(hex)) {
    throw new Error(`Relocation aggregate '${value}' is not a canonical 128-bit identity.`);
  }
  const result = {
    high: BigInt(`0x${hex.slice(0, 16)}`),
    low: BigInt(`0x${hex.slice(16)}`)
  };
  if (result.high === 0n && result.low === 0n) {
    throw new Error('Relocation identity must not be zero.');
  }
  return result;
}

export function createServiceRelocationId(
  isInUse: (candidate: string) => boolean,
  entropy: (size: number) => Uint8Array = randomBytes
): string {
  for (;;) {
    const bytes = entropy(16);
    if (bytes.byteLength !== 16) {
      throw new Error('Relocation identity entropy must return exactly 16 bytes.');
    }
    let nonZero = false;
    for (const byte of bytes) {
      if (byte !== 0) {
        nonZero = true;
        break;
      }
    }
    if (!nonZero) continue;
    const hex = Buffer.from(bytes).toString('hex');
    const candidate = `${hex.slice(0, 8)}-${hex.slice(8, 12)}-${hex.slice(12, 16)}-`
      + `${hex.slice(16, 20)}-${hex.slice(20)}`;
    if (!isInUse(candidate)) return candidate;
  }
}

function operationWireId(value: string): { readonly high: bigint; readonly low: bigint } {
  const match = /^(\d+):(\d+)$/.exec(value);
  if (match === null) throw new Error(`Actor handoff operation '${value}' is not canonical.`);
  const result = { high: BigInt(match[1]!), low: BigInt(match[2]!) };
  if (result.high === 0n && result.low === 0n) {
    throw new Error('Actor handoff operation identity must not be zero.');
  }
  return result;
}

function sameWireId(
  left: { readonly high: bigint; readonly low: bigint },
  right: { readonly high: bigint; readonly low: bigint }
): boolean {
  return left.high === right.high && left.low === right.low;
}

function sameCoordinator(
  left: ServiceWireRelocationCoordinatorFence,
  right: ServiceWireRelocationCoordinatorFence
): boolean {
  return left.ownerId === right.ownerId
    && left.leaseGeneration === right.leaseGeneration
    && left.nodeRid === right.nodeRid
    && left.nodeGeneration === right.nodeGeneration
    && left.expectedAuthorityStoreVersion === right.expectedAuthorityStoreVersion;
}

function isExactSourceAuthority(
  current: ZLinkAuthoritySnapshot,
  expected: ZLinkAuthoritySnapshot
): boolean {
  return current.storeVersion.value === expected.storeVersion.value
    && current.objectGeneration === expected.objectGeneration
    && current.authorityOwnerGeneration === expected.authorityOwnerGeneration
    && current.ownerId === expected.ownerId
    && current.ownerLeaseGeneration === expected.ownerLeaseGeneration
    && current.allocation.state === expected.allocation.state
    && current.allocation.objectKind === expected.allocation.objectKind
    && current.allocation.stableType === expected.allocation.stableType
    && String(current.allocation.descriptor.rid)
      === String(expected.allocation.descriptor.rid)
    && current.allocation.descriptorLifecycleGeneration
      === expected.allocation.descriptorLifecycleGeneration;
}

function isExactCommittedTarget(
  current: ZLinkAuthoritySnapshot,
  expected: ZLinkAuthoritySnapshot,
  target: ServiceWireRelocationTarget,
  payload: Uint8Array
): boolean {
  return current.objectGeneration === expected.objectGeneration
    && current.authorityOwnerGeneration > expected.authorityOwnerGeneration
    && current.ownerId === target.ownerId
    && current.ownerLeaseGeneration === target.ownerLeaseGeneration
    && String(current.allocation.descriptor.rid) === target.nodeRid
    && current.allocation.descriptorLifecycleGeneration === target.nodeGeneration
    && Buffer.from(current.payload).equals(Buffer.from(payload));
}

async function waitForLocationRetry(
  deadlineAtMs: number,
  signal: AbortSignal | undefined,
  cause: unknown
): Promise<void> {
  signal?.throwIfAborted();
  if (Date.now() >= deadlineAtMs) {
    throw new Error(
      'location_update_failed: relocation Restore validity expired before exact target owner confirmation.',
      { cause }
    );
  }
  await new Promise<void>((resolve, reject) => {
    let timer: ReturnType<typeof setTimeout> | undefined;
    const aborted = () => {
      if (timer !== undefined) clearTimeout(timer);
      reject(signal?.reason);
    };
    signal?.addEventListener('abort', aborted, { once: true });
    timer = setTimeout(() => {
      signal?.removeEventListener('abort', aborted);
      resolve();
    }, Math.min(25, Math.max(1, deadlineAtMs - Date.now())));
    timer.unref();
  });
}

function sameRequestSource(
  left: ServiceWireRequestSourceFence,
  right: ServiceWireRequestSourceFence
): boolean {
  return left.ownerId === right.ownerId
    && left.leaseGeneration === right.leaseGeneration
    && left.nodeRid === right.nodeRid
    && left.nodeGeneration === right.nodeGeneration;
}

function handoffSourceFence(
  value: NonNullable<ZLinkActorHandoffPacket['source']>
): ServiceWireRequestSourceFence {
  return {
    ownerId: value.ownerId,
    leaseGeneration: BigInt(value.ownerLeaseGeneration),
    nodeRid: value.nodeRid,
    nodeGeneration: BigInt(value.nodeGeneration)
  };
}

function maintenanceReplyRelay(
  stage: LocalStage,
  coordinator: ServiceWireRelocationCoordinatorFence,
  participantId: bigint,
  completion: RelocationTerminalCompletion
): ServiceMaintenanceReplyRelay {
  const value = completion.result.ok ? completion.result.response : undefined;
  return {
    relocation: relocationWireId(stage.staging.envelope.aggregateId),
    targetAttemptGeneration: stage.staging.envelope.aggregateGeneration,
    coordinator,
    operation: operationWireId(completion.operationId),
    replyRouteId: BigInt(completion.source.replyRouteId),
    participantId,
    sequence: BigInt(completion.index + 1),
    terminalResult: completion.result.ok ? 0 : 105,
    failureCode: completion.result.ok ? 0 : 17,
    ...(value === undefined ? {} : { payload: {
      packetName: 'zlink.relocation.reply',
      contentType: 'application/json',
      bytes: Buffer.from(JSON.stringify(value), 'utf8')
    } })
  };
}

function handoffResultFromRelay(relay: ServiceMaintenanceReplyRelay): ZLinkActorHandoffResult {
  if (relay.sequence > BigInt(Number.MAX_SAFE_INTEGER)) {
    throw new TypeError('Relocation terminal sequence exceeds the Node index range.');
  }
  let value: unknown;
  if (relay.payload !== undefined) {
    if (relay.terminalResult !== 0
      || relay.payload.packetName !== 'zlink.relocation.reply'
      || relay.payload.contentType !== 'application/json') {
      throw new TypeError('Relocation terminal payload boundary is invalid.');
    }
    value = JSON.parse(Buffer.from(relay.payload.bytes).toString('utf8')) as unknown;
  }
  const index = Number(relay.sequence - 1n);
  return relay.terminalResult === 0
    ? { index, ok: true, ...(value === undefined ? {} : { response: value }) }
    : { index, ok: false, error: `Actor handoff failed with framework code ${relay.failureCode}.` };
}

function wireIdText(value: { readonly high: bigint; readonly low: bigint }): string {
  return `${value.high}:${value.low}`;
}

function relocationStagingId(value: {
  readonly relocation: { readonly high: bigint; readonly low: bigint };
  readonly targetAttemptGeneration: bigint;
}): string {
  return `${value.relocation.high}:${value.relocation.low}:${value.targetAttemptGeneration}`;
}

function coordinatorSource(value: ServiceWireRelocationCoordinatorFence) {
  return { ownerId: value.ownerId, leaseGeneration: value.leaseGeneration,
    nodeRid: value.nodeRid, nodeGeneration: value.nodeGeneration };
}

function relocationObject(envelope: ServiceRelocationEnvelope): ServiceWireRelocationObject {
  const participant = envelope.participants.find(value => value.objectKind !== 'actor')
    ?? envelope.participants[0]!;
  const identity = decodeAuthorityKey({ value: participant.key } as ZLinkAuthorityKey).globalId;
  if (participant.objectKind === 'actor') {
    return { kind: 'actor', actorId: identity, objectGeneration: participant.objectGeneration,
      expectedAuthorityOwnerGeneration: participant.authorityOwnerGeneration };
  }
  if (participant.objectKind === 'user_spot') {
    return { kind: 'userSpot', spotId: identity, objectGeneration: participant.objectGeneration,
      expectedAuthorityOwnerGeneration: participant.authorityOwnerGeneration };
  }
  return { kind: 'instanceSpot', stableType: participant.stableType,
    spotId: identity, objectGeneration: participant.objectGeneration };
}

function relocationObjectForParticipant(
  participant: ServiceRelocationParticipant
): ServiceWireRelocationObject {
  const identity = decodeAuthorityKey(
    { value: participant.key } as ZLinkAuthorityKey
  ).globalId;
  if (participant.objectKind === 'actor') {
    return {
      kind: 'actor',
      actorId: identity,
      objectGeneration: participant.objectGeneration,
      expectedAuthorityOwnerGeneration: participant.authorityOwnerGeneration
    };
  }
  if (participant.objectKind === 'user_spot') {
    return {
      kind: 'userSpot',
      spotId: identity,
      objectGeneration: participant.objectGeneration,
      expectedAuthorityOwnerGeneration: participant.authorityOwnerGeneration
    };
  }
  return {
    kind: 'instanceSpot',
    stableType: participant.stableType,
    spotId: identity,
    objectGeneration: participant.objectGeneration
  };
}

function relocationReady(
  request: ServiceMaintenanceRelocationPrepare
): ServiceMaintenanceRelocationReady {
  return {
    kind: 'ready',
    relocation: request.relocation,
    targetAttemptGeneration: request.targetAttemptGeneration,
    coordinator: request.coordinator,
    target: request.target,
    object: request.object,
    senderRole: 'target'
  };
}

function validatePrepareEnvelope(
  request: ServiceMaintenanceRelocationPrepare,
  envelope: ServiceRelocationEnvelope
): void {
  if (!sameWireId(request.relocation, relocationWireId(envelope.aggregateId))
    || request.targetAttemptGeneration !== envelope.aggregateGeneration
    || stringifyWire(request.object) !== stringifyWire(relocationObject(envelope))) {
    throw new Error('Relocation Prepare does not match its immutable root identity.');
  }
}

function validateTargetOneWayControl(
  stage: LocalStage,
  request: ServiceMaintenanceRelocationData | ServiceMaintenanceRelocationCutover,
  sourceNodeRid: RoutingId
): void {
  const prepare = stage.offer.prepare;
  if (String(sourceNodeRid) !== stage.offer.authenticatedSourceNodeRid
    || !sameWireId(request.relocation, prepare.relocation)
    || request.targetAttemptGeneration !== prepare.targetAttemptGeneration
    || !sameCoordinator(request.coordinator, prepare.coordinator)
    || stringifyWire(request.object) !== stringifyWire(
      request.kind === 'data' ? request.object : prepare.object
    )) {
    throw new Error('Relocation one-way control changed its exact Prepare fence.');
  }
  if (request.kind === 'cutover'
    && stringifyWire(request.object) !== stringifyWire(prepare.object)) {
    throw new Error('Relocation Cutover changed its prepared object.');
  }
}

function relocationPublication(
  prepare: ServiceMaintenanceRelocationPrepare,
  envelope: ServiceRelocationEnvelope
): ServiceRelocationPublication {
  if (prepare.root === undefined) {
    throw new Error('Relocation Prepare has no immutable root.');
  }
  return {
    reference: prepare.root.reference,
    checksumCrc32c: prepare.root.checksumCrc32c,
    aggregateId: envelope.aggregateId,
    aggregateGeneration: envelope.aggregateGeneration,
    inventoryDigest: inventoryDigest(envelope.participants, envelope.memberships),
    targetOwnerId: prepare.target.ownerId,
    targetOwnerLeaseGeneration: prepare.target.ownerLeaseGeneration
  };
}

function canonicalRelayFrozenRecord(
  participant: ServiceRelocationParticipant,
  packet: ZLinkActorHandoffPacket,
  coordinator: ServiceWireRelocationCoordinatorFence,
  target: ServiceWireRelocationTarget
) {
  const message = encodeHandoffQueuedMessages([packet])[0]!;
  return canonicalQueuedFrozenRecord(participant, message, coordinator, target);
}

function raceAbort<T>(operation: Promise<T>, signal: AbortSignal): Promise<T> {
  if (signal.aborted) return Promise.reject(signal.reason);
  return new Promise<T>((resolve, reject) => {
    const aborted = () => reject(signal.reason);
    signal.addEventListener('abort', aborted, { once: true });
    operation.then(
      value => {
        signal.removeEventListener('abort', aborted);
        resolve(value);
      },
      error => {
        signal.removeEventListener('abort', aborted);
        reject(error);
      }
    );
  });
}

async function mapConcurrentOrdered<TInput, TResult>(
  values: readonly TInput[],
  concurrency: number,
  callback: (value: TInput, index: number) => Promise<TResult>,
  signal?: AbortSignal
): Promise<TResult[]> {
  const results = new Array<TResult>(values.length);
  let next = 0;
  let failure: unknown;
  const worker = async (): Promise<void> => {
    while (failure === undefined) {
      signal?.throwIfAborted();
      const index = next++;
      if (index >= values.length) return;
      try {
        results[index] = await callback(values[index]!, index);
      } catch (error) {
        failure = error;
      }
    }
  };
  await Promise.all(Array.from(
    { length: Math.min(concurrency, values.length) },
    () => worker()
  ));
  if (failure !== undefined) throw failure;
  return results;
}

function canonicalQueuedFrozenRecord(
  participant: ServiceRelocationParticipant,
  message: ServiceRelocationQueuedMessage,
  coordinator: ServiceWireRelocationCoordinatorFence,
  target: ServiceWireRelocationTarget
) {
  if (participant.objectKind !== 'actor') {
    throw new Error('Only Actor object-mailbox participants can contain accepted packets.');
  }
  const packet = decodeQueuedHandoffPacket(message);
  const identity = decodeAuthorityKey({ value: participant.key } as ZLinkAuthorityKey).globalId;
  const source = packet.source === undefined
    ? coordinatorSource(coordinator)
    : {
        ownerId: packet.source.ownerId,
        leaseGeneration: BigInt(packet.source.ownerLeaseGeneration),
        nodeRid: packet.source.nodeRid,
        nodeGeneration: BigInt(packet.source.nodeGeneration)
      };
  const operationId = parseHandoffOperationId(
    packet.messageFollowContext.operationId
  );
  if (packet.returnResponse && packet.source === undefined) {
    throw new Error('Captured Actor request has no exact request-source fence.');
  }
  if (!packet.returnResponse && packet.source === undefined
    && packet.remoteBoundSessionTarget !== undefined) {
    // A durable frozen record is permitted only for a lease-backed source.
    // Work tied to a Session connection's lifetime drains before Captured,
    // so freezing it under the coordinator's fence is a protocol error.
    throw new Error('Captured connection-bound Actor send cannot enter a relocation envelope.');
  }
  return encodeServiceWireFrozenActorApplicationRecord({
    source,
    target: {
      actorId: identity,
      objectGeneration: participant.objectGeneration,
      nodeRid: target.nodeRid,
      nodeGeneration: target.nodeGeneration,
      authorityOwnerGeneration: participant.authorityOwnerGeneration + 1n,
      ownerLeaseGeneration: target.ownerLeaseGeneration
    },
    operationId,
    ...(packet.returnResponse ? { replyRouteId: BigInt(packet.source!.replyRouteId) } : {}),
    payload: {
      packetName: '__zlink.actor.handoff.accepted',
      contentType: 'application/json',
      bytes: message.payload
    }
  });
}

function parseHandoffOperationId(value: string): { readonly high: bigint; readonly low: bigint } {
  if (!/^[0-9a-f]{32}$/.test(value)) {
    throw new Error('Captured Actor operation ID is invalid.');
  }
  const operation = {
    high: BigInt(`0x${value.slice(0, 16)}`),
    low: BigInt(`0x${value.slice(16)}`)
  };
  if (operation.high === 0n && operation.low === 0n) {
    throw new Error('Captured Actor operation ID must not be zero.');
  }
  return operation;
}

function stringifyWire(value: unknown): string {
  return JSON.stringify(value, (_key, item) => typeof item === 'bigint' ? item.toString() : item);
}

function validateControlResponse(
  request: ZLinkServiceRelocationControlRequest,
  response: ZLinkServiceRelocationControlResponse
): void {
  if (request.kind !== 'prepare'
    || response.kind !== 'ready'
    || !sameWireId(response.relocation, request.relocation)
    || response.targetAttemptGeneration !== request.targetAttemptGeneration
    || !sameCoordinator(response.coordinator, request.coordinator)
    || stringifyWire(response.target) !== stringifyWire(request.target)
    || stringifyWire(response.object) !== stringifyWire(request.object)
    || response.senderRole !== 'target') {
    throw new Error('Relocation control response does not match the request fence.');
  }
}

function controlAckKey(request: ZLinkServiceRelocationControlRequest): string {
  if (request.kind !== 'prepare') {
    throw new Error(`Relocation ${request.kind} is one-way and has no response key.`);
  }
  return `relocation:${request.relocation.high}:${request.relocation.low}:`
    + `${request.targetAttemptGeneration}:ready`;
}

function controlResponseKey(packet: ZLinkServiceRelocationControlRequest): string | undefined {
  if (packet.kind === 'ready' && packet.senderRole === 'target') {
    return `relocation:${packet.relocation.high}:${packet.relocation.low}:${packet.targetAttemptGeneration}:ready`;
  }
  return undefined;
}

function sessionRelocationPendingKey(request: ServiceSessionRelocationSeal): string {
  return sessionRelocationIdentityKey(request, request.actor.actor);
}

function sessionRelocationResponseKey(response: ServiceSessionRelocationSealed): string {
  return sessionRelocationIdentityKey(response, response.actor.actor);
}

function sessionRelocationResponseFingerprint(response: ServiceSessionRelocationSealed): string {
  return encodeSessionRelocationSealed(response).toString('base64');
}

function sessionRelocationIdentityKey(
  value: {
    readonly relocation: ServiceWireOperationId;
    readonly session: { readonly sessionRid: string; readonly bindingGeneration: bigint };
  },
  actor: { readonly actorId: string; readonly generation: bigint }
): string {
  return [
    'session-relocation',
    value.relocation.high.toString(),
    value.relocation.low.toString(),
    actor.actorId,
    actor.generation.toString(),
    value.session.sessionRid,
    value.session.bindingGeneration.toString(),
    'sealed'
  ].join(':');
}

function validateSessionRelocationResponse(
  request: ServiceSessionRelocationSeal,
  response: ServiceSessionRelocationSealed
): void {
  const sameCoordinator =
    request.coordinator.ownerId === response.coordinator.ownerId
    && request.coordinator.leaseGeneration === response.coordinator.leaseGeneration
    && request.coordinator.nodeRid === response.coordinator.nodeRid
    && request.coordinator.nodeGeneration === response.coordinator.nodeGeneration
    && request.coordinator.expectedAuthorityStoreVersion
      === response.coordinator.expectedAuthorityStoreVersion;
  const sameSession =
    request.session.sessionOwnerNodeRid === response.session.sessionOwnerNodeRid
    && request.session.sessionOwnerNodeGeneration
      === response.session.sessionOwnerNodeGeneration
    && request.session.sessionOwnerId === response.session.sessionOwnerId
    && request.session.sessionOwnerLeaseGeneration
      === response.session.sessionOwnerLeaseGeneration
    && request.session.sessionRid === response.session.sessionRid
    && request.session.bindingGeneration === response.session.bindingGeneration;
  if (
    request.relocation.high !== response.relocation.high
    || request.relocation.low !== response.relocation.low
    || !sameCoordinator
    || !sameSession
  ) {
    throw new ServiceWireProtocolError('Session relocation ACK changed its exact fence.');
  }
  if (
    request.actor.actor.actorId !== response.actor.actor.actorId
    || request.actor.actor.generation !== response.actor.actor.generation
    || request.actor.actor.nodeRid !== response.actor.actor.nodeRid
    || request.actor.targetNodeGeneration !== response.actor.targetNodeGeneration
    || request.actor.authorityOwnerGeneration !== response.actor.authorityOwnerGeneration
    || request.actor.ownerLeaseGeneration !== response.actor.ownerLeaseGeneration
  ) {
    throw new ServiceWireProtocolError('Command 43 ACK does not echo command 42.');
  }
}

function replyRelayIdentityKey(value: {
  readonly relocation: { readonly high: bigint; readonly low: bigint };
  readonly coordinator: ServiceWireRelocationCoordinatorFence;
  readonly operation: { readonly high: bigint; readonly low: bigint };
  readonly replyRouteId: bigint;
}): string {
  return JSON.stringify([
    value.operation.high.toString(),
    value.operation.low.toString(),
    value.replyRouteId.toString(),
    value.relocation.high.toString(),
    value.relocation.low.toString(),
    value.coordinator.ownerId,
    value.coordinator.leaseGeneration.toString(),
    value.coordinator.nodeRid,
    value.coordinator.nodeGeneration.toString(),
    value.coordinator.expectedAuthorityStoreVersion
  ]);
}

function replyRelayPendingKey(
  ackTargetNodeRid: string,
  value: Parameters<typeof replyRelayIdentityKey>[0]
): string {
  return JSON.stringify([ackTargetNodeRid, replyRelayIdentityKey(value)]);
}

function isServiceWireCommand(payload: Uint8Array, command: number): boolean {
  return payload.byteLength >= 5
    && payload[0] === 0x5a && payload[1] === 0x4d && payload[2] === 1
    && payload[3] === command;
}

function isRecord(value: unknown): value is Record<string, unknown> {
  return typeof value === 'object' && value !== null && !Array.isArray(value);
}

function encodeHandoffQueuedMessages(
  backlog: readonly ZLinkActorHandoffPacket[]
): readonly ServiceRelocationQueuedMessage[] {
  return backlog.map(packet => ({
    sequence: BigInt(packet.index) + 1n,
    payload: Buffer.from(JSON.stringify(packet), 'utf8')
  }));
}

function toServiceTimer(value: import('../spots/spot-timer').ZLinkTimerRelocationState): ServiceRelocationTimer {
  return {
    timerId: value.name,
    startedAtUnixMs: value.startedAtUnixMs,
    dueAtUnixMs: value.startedAtUnixMs + Number(value.lastScheduledIndex + 1n) * value.periodMs,
    intervalMs: value.periodMs,
    deliveryIndex: value.deliveryIndex,
    lastScheduledIndex: value.lastScheduledIndex,
    overrunPolicy: value.overrunPolicy,
    maxCatchUpTicks: value.maxCatchUpTicks,
    stopOnUnhandledException: value.stopOnUnhandledException,
    pendingTicks: value.pendingTicks
  };
}

function encodeActorSession(target: ZLinkRemoteBoundSessionTarget | undefined): Buffer {
  if (target === undefined) return Buffer.alloc(0);
  return Buffer.from(JSON.stringify({
    version: 1,
    routerChannelId: target.routerChannelId,
    targetNodeRid: String(target.targetNodeRid),
    spotId: String(target.spotId),
    sessionNodeRid: target.sessionNodeRid === undefined ? undefined : String(target.sessionNodeRid),
    sessionRid: target.sessionRid === undefined ? undefined : String(target.sessionRid),
    bindingGeneration: target.bindingGeneration?.toString(),
    previousAuthorityOwnerGeneration: target.previousAuthorityOwnerGeneration?.toString(),
    previousOwnerLeaseGeneration: target.previousOwnerLeaseGeneration?.toString(),
    relocationSealId: target.relocationSealId,
    serviceWireRelocation: target.serviceWireRelocation === undefined
      ? undefined
      : {
          relocationHigh: target.serviceWireRelocation.relocation.high.toString(),
          relocationLow: target.serviceWireRelocation.relocation.low.toString(),
          coordinatorOwnerId: target.serviceWireRelocation.coordinator.ownerId,
          coordinatorLeaseGeneration:
            target.serviceWireRelocation.coordinator.leaseGeneration.toString(),
          coordinatorNodeRid: target.serviceWireRelocation.coordinator.nodeRid,
          coordinatorNodeGeneration:
            target.serviceWireRelocation.coordinator.nodeGeneration.toString(),
          expectedAuthorityStoreVersion:
            target.serviceWireRelocation.coordinator.expectedAuthorityStoreVersion,
          sessionOwnerNodeRid: target.serviceWireRelocation.session.sessionOwnerNodeRid,
          sessionOwnerNodeGeneration:
            target.serviceWireRelocation.session.sessionOwnerNodeGeneration.toString(),
          sessionOwnerId: target.serviceWireRelocation.session.sessionOwnerId,
          sessionOwnerLeaseGeneration:
            target.serviceWireRelocation.session.sessionOwnerLeaseGeneration.toString(),
          sessionRid: target.serviceWireRelocation.session.sessionRid,
          bindingGeneration:
            target.serviceWireRelocation.session.bindingGeneration.toString()
        }
  }), 'utf8');
}

function decodeActorSession(payload: Uint8Array): ZLinkRemoteBoundSessionTarget {
  const value = JSON.parse(Buffer.from(payload).toString('utf8')) as Record<string, unknown>;
  const optionalBigInt = (field: string) => typeof value[field] === 'string'
    ? BigInt(value[field] as string)
    : undefined;
  if (value.version !== 1 || typeof value.routerChannelId !== 'string'
    || typeof value.targetNodeRid !== 'string' || typeof value.spotId !== 'string') {
    throw new TypeError('Actor relocation Session journal is invalid.');
  }
  const serviceWire = decodeActorSessionServiceWireFence(value.serviceWireRelocation);
  return {
    routerChannelId: value.routerChannelId,
    targetNodeRid: value.targetNodeRid as RoutingId,
    spotId: value.spotId as RoutingId,
    ...(typeof value.sessionNodeRid === 'string' ? { sessionNodeRid: value.sessionNodeRid as RoutingId } : {}),
    ...(typeof value.sessionRid === 'string' ? { sessionRid: value.sessionRid as RoutingId } : {}),
    ...(optionalBigInt('bindingGeneration') === undefined ? {} : { bindingGeneration: optionalBigInt('bindingGeneration') }),
    ...(optionalBigInt('previousAuthorityOwnerGeneration') === undefined ? {} : { previousAuthorityOwnerGeneration: optionalBigInt('previousAuthorityOwnerGeneration') }),
    ...(optionalBigInt('previousOwnerLeaseGeneration') === undefined ? {} : { previousOwnerLeaseGeneration: optionalBigInt('previousOwnerLeaseGeneration') }),
    ...(typeof value.relocationSealId === 'string' ? { relocationSealId: value.relocationSealId } : {}),
    ...(serviceWire === undefined ? {} : { serviceWireRelocation: serviceWire })
  };
}

function decodeActorSessionServiceWireFence(
  input: unknown
): NonNullable<ZLinkRemoteBoundSessionTarget['serviceWireRelocation']> | undefined {
  if (input === undefined) return undefined;
  if (typeof input !== 'object' || input === null) {
    throw new TypeError('Actor relocation Session service-wire fence is invalid.');
  }
  const value = input as Record<string, unknown>;
  const text = (field: string) => {
    if (typeof value[field] !== 'string' || (value[field] as string).length === 0) {
      throw new TypeError(`Actor relocation Session service-wire '${field}' is invalid.`);
    }
    return value[field] as string;
  };
  const positive = (field: string) => {
    const parsed = BigInt(text(field));
    if (parsed <= 0n) {
      throw new TypeError(`Actor relocation Session service-wire '${field}' is invalid.`);
    }
    return parsed;
  };
  const ordinal = (field: string) => {
    const parsed = BigInt(text(field));
    if (parsed < 0n) {
      throw new TypeError(`Actor relocation Session service-wire '${field}' is invalid.`);
    }
    return parsed;
  };
  const relocation = {
    high: ordinal('relocationHigh'),
    low: ordinal('relocationLow')
  };
  if (relocation.high === 0n && relocation.low === 0n) {
    throw new TypeError('Actor relocation Session service-wire identity is zero.');
  }
  return {
    relocation,
    coordinator: {
      ownerId: text('coordinatorOwnerId'),
      leaseGeneration: positive('coordinatorLeaseGeneration'),
      nodeRid: text('coordinatorNodeRid'),
      nodeGeneration: positive('coordinatorNodeGeneration'),
      expectedAuthorityStoreVersion: text('expectedAuthorityStoreVersion')
    },
    session: {
      sessionOwnerNodeRid: text('sessionOwnerNodeRid'),
      sessionOwnerNodeGeneration: positive('sessionOwnerNodeGeneration'),
      sessionOwnerId: text('sessionOwnerId'),
      sessionOwnerLeaseGeneration: positive('sessionOwnerLeaseGeneration'),
      sessionRid: text('sessionRid'),
      bindingGeneration: positive('bindingGeneration')
    }
  };
}

function encodeMembershipMutation(
  memberships: readonly ServiceRelocationMembership[],
  participantKey: string
): Buffer {
  return Buffer.from(JSON.stringify(memberships.filter(value =>
    value.actorKey === participantKey || value.spotKey === participantKey).map(value => ({
      ...value,
      spotObjectGeneration: value.spotObjectGeneration.toString(),
      membershipEpoch: value.membershipEpoch.toString()
    }))), 'utf8');
}

function addCapacity(
  left: ZLinkCapacityVector,
  right: ZLinkCapacityVector
): ZLinkCapacityVector {
  const spotType = left.spotType ?? right.spotType;
  if (left.spotType !== undefined && right.spotType !== undefined
    && (left.spotType.objectKind !== right.spotType.objectKind
      || left.spotType.stableType !== right.spotType.stableType)) {
    throw new Error('Relocation aggregate contains more than one Spot stable type.');
  }
  return {
    actors: left.actors + right.actors,
    spots: left.spots + right.spots,
    ...(spotType === undefined ? {} : {
      spotType: {
        ...spotType,
        count: (left.spotType?.count ?? 0) + (right.spotType?.count ?? 0)
      }
    })
  };
}
