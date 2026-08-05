import { randomUUID } from 'node:crypto';
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
  ZLinkLocationOwnerToken,
  ZLinkRelocationCapacityFence
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
import type { ZLinkDomainLocationStore as ZLinkLocationStore } from '../locations/domain-store-contract';
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
import type { ReceiveRecord } from '../foundation/service-runtime-contracts';
import type { ServiceSpotMessageFollowSeal } from '../foundation/service-stateful-runtime';
import {
  ServiceDurableRelocationRuntime,
  ServiceRelocationAuthorityPayloadCodec,
  crc32c,
  decodeServiceRelocationEnvelope,
  encodeServiceRelocationEnvelope,
  inventoryDigest,
  type ServiceRelocationEnvelope,
  type ServiceRelocationMembership,
  type ServiceRelocationParticipant,
  type ServiceRelocationPublication,
  type ServiceRelocationQueuedMessage,
  type ServiceRelocationStorePort,
  type ServiceRelocationSuccessorProgress,
  type ServiceRelocationTimer
} from '../foundation/service-relocation-runtime';
import {
  ServiceRelocationCoordinator,
  ServiceRelocationPostCommitError,
  ServiceRelocationSourceAuthorityWriter,
  type ServiceRelocationAuthorityCommit,
  type ServiceRelocationRestoreOwner
} from '../foundation/service-relocation-coordinator';
import {
  ServiceMaintenanceRuntime
} from '../foundation/service-maintenance-runtime';
import {
  ServiceCapturedRelocationSourceCompletion,
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
import type { DefaultZLinkSpotManager } from '../spots';
import type { ZLinkSpotActivation } from '../spots/spot-activation-state';
import type { DefaultZLinkActorManager } from '../actors';
import type { ZLinkActorRuntimeState, ZLinkRemoteBoundSessionTarget } from '../actors/actor-runtime-state';
import {
  replayActorHandoffBacklog,
  type ZLinkActorHandoffPacket,
  type ZLinkActorHandoffResult
} from '../actors/actor-handoff';
import { decodeHandoffBacklog } from '../spots/spot-remote-codec';
import type { ZLinkActorTransferRuntime } from './actor-transfer-runtime';
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
  encodeMaintenanceReplyRelay,
  encodeMaintenanceReplyRelayAck,
  encodeServiceWireFrozenActorApplicationRecord,
  M6bServiceWireCommand,
  type ServiceMaintenanceReplyRelay,
  type ServiceMaintenanceReplyRelayAck,
  type ServiceMaintenanceRelocationComplete,
  type ServiceMaintenanceRelocationControl,
  type ServiceMaintenanceRelocationReady,
  type ServiceMaintenanceRelocationControlData,
  type ServiceMaintenanceRelocationPrepare,
  type ServiceWireRequestSourceFence,
  type ServiceWireRelocationCandidate,
  type ServiceWireRelocationCoordinatorFence,
  type ServiceWireRelocationObject,
  type ServiceWireRelocationParticipant
} from '../foundation/service-stateful-wire-codec';

const RELOCATION_MESSAGE_ESTIMATE_PER_UNIT = 64n;
const RELOCATION_STRIPE_BYTES = 64 * 1024 * 1024;
const RELOCATION_INGRESS_HOLD_BYTES = 16 * 1024 * 1024;
const DEFAULT_RELOCATION_PAYLOAD_ESTIMATE_BYTES =
  RELOCATION_STRIPE_BYTES + RELOCATION_INGRESS_HOLD_BYTES;
const RELOCATION_STRIPE_CONCURRENCY = 64;
const RELOCATION_STRIPE_LIMIT = 4096;
const RELOCATION_STRIPE_MANIFEST_KIND = 'zlink-relocation-striped-v1';
const RELOCATION_TARGET_LIVE_LIMIT = 1024;
const RELOCATION_TARGET_TOMBSTONE_LIMIT = 1024;
const RELOCATION_TARGET_TOMBSTONE_TTL_MS = 5 * 60_000;

class TargetReservationRejectedError extends Error {}

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
  readonly actorManager: () => DefaultZLinkActorManager | undefined;
  readonly actorTransfer: ZLinkActorTransferRuntime;
  readonly runtimeEventPublisher?: ZLinkRuntimeEventPublisher;
  readonly metrics?: ZLinkRuntimeMetrics;
}

interface RemoteStaging {
  readonly id: string;
  readonly meshName: string;
  readonly targetNodeRid: string;
  readonly envelope: ServiceRelocationEnvelope;
  readonly relocation: { readonly high: bigint; readonly low: bigint };
  readonly targetAttemptGeneration: bigint;
  readonly candidate: ServiceWireRelocationCandidate;
  readonly object: ServiceWireRelocationObject;
  readonly participants: readonly ServiceWireRelocationParticipant[];
  readonly root: NonNullable<ServiceMaintenanceRelocationPrepare['root']>;
}

interface RemotePreReservation {
  readonly owner: RemoteRestoreOwner;
  readonly manifestRoot: string;
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
  readonly terminalDeliveries: Map<number, 'terminalReceived' | 'alreadyTerminal' | 'sourceLeaseExpired'>;
  targetAuthorityOwnerGeneration?: bigint;
}

interface LocalStage {
  readonly offer: TargetRelocationOffer;
  readonly owner: ServiceRelocationObjectRestoreOwner<LocalHidden>;
  readonly staging: ServiceObjectRelocationStaging<LocalHidden>;
  readonly coordinator: Extract<
    ServiceWireRelocationCoordinatorFence,
    ServiceWireRelocationCoordinatorFence
  >;
  readonly candidate: ServiceWireRelocationCandidate;
  phase: 'prepared' | 'published' | 'replayed' | 'sealed' | 'routed' | 'normalized' | 'open';
  terminalRelayed: boolean;
}

type TargetRelocationReservation =
  | { readonly kind: 'single'; readonly fence: ZLinkRelocationCapacityFence }
  | {
      readonly kind: 'aggregate';
      readonly prepared: ServicePreparedRelocationAggregate;
    };

interface TargetRelocationOffer {
  readonly prepare: ServiceMaintenanceRelocationPrepare;
  readonly prepareFingerprint: string;
  readonly authenticatedSourceNodeRid: string;
  readonly envelope: ServiceRelocationEnvelope;
  readonly offeredMessages: bigint;
  readonly offeredBytes: bigint;
  expiryTimer?: ReturnType<typeof setTimeout>;
  permit?: {
    messages: bigint;
    bytes: bigint;
    readonly active: boolean;
    released: boolean;
  };
  capacityReservation?: Promise<Extract<TargetRelocationReservation, { readonly kind: 'single' }>>;
  reservation?: Promise<TargetRelocationReservation>;
  reconcileReservation?: (signal?: AbortSignal) => Promise<TargetRelocationReservation>;
  reservationCommitted?: boolean;
  materialization?: Promise<LocalStage>;
}

interface SourceActorSession {
  readonly state: ZLinkActorRuntimeState;
  readonly actor: ZLinkActor;
  readonly prepared: Awaited<ReturnType<ZLinkActorTransferRuntime['prepareMaintenanceSession']>>;
}

interface PendingRelocationControl {
  readonly targetNodeRid: string;
  readonly request: ZLinkServiceRelocationControlRequest;
  readonly resolve: (response: ZLinkServiceRelocationControlResponse) => void;
  readonly reject: (error: unknown) => void;
  readonly timer: ReturnType<typeof setInterval>;
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

/** Production host bridge from Retire inventory to remote RouteMesh owners. */
export class ZLinkHostServiceRelocationRuntime {
  private readonly targetOffers = new Map<string, TargetRelocationOffer>();
  private readonly targetStages = new Map<string, LocalStage>();
  private readonly targetAborts = new Map<string, {
    readonly fingerprint: string;
    readonly authenticatedSourceNodeRid: string;
    readonly expiresAtMs: number;
  }>();
  private readonly completedTargets = new Map<string, {
    readonly fingerprint: string;
    readonly authenticatedSourceNodeRid: string;
    readonly response: Extract<ServiceMaintenanceRelocationControl, { kind: 'complete' }>;
    readonly expiresAtMs: number;
  }>();
  private reservedTargetMessages = 0n;
  private reservedTargetBytes = 0n;
  private activeTargetRelocations = 0;
  private readonly relocationAuthorityKeys = new Map<string, string>();
  private readonly pendingControls = new Map<string, PendingRelocationControl>();
  private readonly pendingReplyRelays = new Map<string, PendingRelocationReplyRelay>();
  private readonly codec = new ServiceRelocationAuthorityPayloadCodec();
  private readonly recoveredPublications = new Set<string>();

  constructor(private readonly options: ZLinkHostRelocationOptions) {}

  async dispose(): Promise<void> {
    const offers = new Set<TargetRelocationOffer>([
      ...this.targetOffers.values(),
      ...[...this.targetStages.values()].map(stage => stage.offer)
    ]);
    const errors: unknown[] = [];
    for (const offer of offers) {
      if (offer.expiryTimer !== undefined) clearTimeout(offer.expiryTimer);
      try {
        if (offer.reservationCommitted !== true) {
          await this.abortTargetOffer(relocationStagingId(offer.prepare), offer);
        } else {
          this.releaseTargetPermit(offer);
        }
      } catch (error) {
        errors.push(error);
      } finally {
        if (offer.expiryTimer !== undefined) clearTimeout(offer.expiryTimer);
      }
    }
    this.targetOffers.clear();
    this.targetStages.clear();
    this.targetAborts.clear();
    this.completedTargets.clear();
    const stopped = new Error('Relocation runtime stopped.');
    stopped.name = 'AbortError';
    for (const pending of this.pendingControls.values()) {
      clearInterval(pending.timer);
      pending.reject(stopped);
    }
    this.pendingControls.clear();
    for (const pending of this.pendingReplyRelays.values()) {
      if (pending.timer !== undefined) clearTimeout(pending.timer);
      pending.reject(stopped);
    }
    this.pendingReplyRelays.clear();
    this.relocationAuthorityKeys.clear();
    if (errors.length === 1) throw errors[0];
    if (errors.length > 1) throw new AggregateError(errors, 'Relocation runtime stop failed.');
  }

  async relocateMesh(
    meshName: string,
    targetApplicationVersion?: bigint,
    signal?: AbortSignal
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
      readonly participantCount: number;
      readonly allowOversizedExclusive?: boolean;
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
          participantCount: 1,
          relocate: relocateShell
        });
        for (const state of states) {
          work.push({
            id: `actor:${state.actorId}`,
            participantCount: 1,
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
        participantCount: states.length + 1,
        allowOversizedExclusive: kind === 'user_spot',
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
        participantCount: 1,
        relocate: relocationSignal =>
          this.relocateStandaloneActor(
            meshName, state, undefined, targetApplicationVersion, relocationSignal)
      });
    }
    if (work.length === 0) return;
    const limits = this.relocationLimits();
    const maintenance = new ServiceMaintenanceRuntime({
      preflight: async () => true,
      publishState: () => undefined,
      forceStop: () => undefined,
      maxOutbound: limits.maxActiveOutboundRelocations,
      maxInFlightBytes: limits.maxRelocationPayloadInFlightBytes
    });
    for (const unit of work) {
      maintenance.enqueue({
        id: unit.id,
        encodedUpperBound:
          unit.participantCount * DEFAULT_RELOCATION_PAYLOAD_ESTIMATE_BYTES,
        allowOversizedExclusive: unit.allowOversizedExclusive,
        ready: () => true,
        relocate: unit.relocate
      });
    }
    const operationSignal = signal ?? new AbortController().signal;
    const deadlineMs = signal === undefined ? 30_000 : 24 * 60 * 60 * 1000;
    const result = await raceAbort(maintenance.start('retire', deadlineMs), operationSignal);
    if (result.state !== 'completed') {
      throw result.terminalError ?? new Error(`Host relocation ended in '${result.state}'.`);
    }
  }

  async recoverPublishedAuthority(
    authority: ZLinkAuthoritySnapshot,
    signal?: AbortSignal
  ): Promise<void> {
    const publication = this.codec.read(authority.payload);
    if (publication === undefined) return;
    const recoveryId = `${publication.reference}\0${authority.storeVersion.value}`;
    if (this.recoveredPublications.has(recoveryId)) return;
    const currentOwner = this.options.currentOwner();
    if (currentOwner === undefined
      || authority.ownerId !== currentOwner.ownerId
      || authority.ownerLeaseGeneration !== currentOwner.leaseGeneration
      || publication.targetOwnerId !== currentOwner.ownerId
      || publication.targetOwnerLeaseGeneration !== currentOwner.leaseGeneration) {
      return;
    }
    const durable = new ServiceDurableRelocationRuntime(
      this.requireLocationStore(),
      relocationStorePort(this.requireRelocationStore()),
      this.codec
    );
    const envelope = await durable.restore(authority, signal);
    const key = primaryKey(envelope);
    const authorityIdentity = envelope.participants.find(value => value.key === key);
    if (authorityIdentity === undefined
      || authorityIdentity.objectKind !== authority.allocation.objectKind
      || authorityIdentity.stableType !== authority.allocation.stableType
      || authorityIdentity.objectGeneration !== authority.objectGeneration) {
      return;
    }
    if (authorityIdentity.authorityOwnerGeneration >= authority.authorityOwnerGeneration) {
      throw new Error('Published relocation root does not match its primary authority.');
    }
    const meshName = authority.allocation.descriptor.meshName;
    const owner = new ServiceRelocationObjectRestoreOwner(
      new LocalTargetPort(this.options, meshName, envelope),
      value => ({ value } as ZLinkAuthorityKey),
      this.relocationLimits().maxConcurrentRelocationRestores
    );
    const staging = await owner.prepare(envelope, signal);
    const recoveryStage = {
      offer: undefined as never,
      owner,
      staging,
      coordinator: undefined as never,
      candidate: undefined as never,
      phase: 'prepared',
      terminalRelayed: false
    } satisfies LocalStage;
    const coordinator = new ServiceRelocationCoordinator(
      durable,
      owner,
      new ServiceRelocationSourceAuthorityWriter(
        durable,
        value => value.primaryAuthorityKey,
        value => successorProgressFromStaging(value)
      ),
      {
        relayCaptured: async (_staging, completedAuthority, relaySignal) => {
          const completions = await this.readDurableTerminalCompletions(
            recoveryStage,
            completedAuthority,
            relaySignal
          );
          const status = this.requireMeshNode(meshName).status();
          const currentOwner = this.options.currentOwner();
          if (currentOwner === undefined) {
            throw new Error('Relocation recovery target owner is unavailable.');
          }
          await this.relayTerminalReplies(
            meshName,
            recoveryStage,
            {
              ownerId: currentOwner.ownerId,
              leaseGeneration: currentOwner.leaseGeneration,
              nodeRid: String(status.routingId),
              nodeGeneration: status.lifecycleGeneration,
              expectedAuthorityStoreVersion: completedAuthority.storeVersion.value
            },
            completions,
            relaySignal
          );
          const progress = localSuccessorProgress(recoveryStage);
          if (![...progress.values()].some(value => value.terminalReplies.byteLength !== 0)) {
            return completedAuthority;
          }
          const previous = this.codec.read(completedAuthority.payload);
          const advanced = await durable.advanceCompletedProgress(
            { value: key } as ZLinkAuthorityKey,
            completedAuthority,
            progress,
            relaySignal,
            { retainPreviousRoot: envelope.participants.length > 1 }
          );
          await this.completeParticipantAuthorities(
            envelope,
            key,
            advanced,
            relaySignal
          );
          if (envelope.participants.length > 1 && previous !== undefined) {
            await durable.deleteRetainedRoot(previous.reference, relaySignal);
          }
          return advanced;
        }
      },
      {
        replace: async () => this.publishSessionRoutes(staging)
      },
      {
        waitUntilReleasable: async (_staging, _authority, recoverySignal) =>
          this.releaseParticipantAuthorities(envelope, key, recoverySignal)
      }
    );
    try {
      await coordinator.resumeCommitted(
        { value: key } as ZLinkAuthorityKey,
        { staging, authority },
        signal
      );
      this.recoveredPublications.add(recoveryId);
    } catch (error) {
      await owner.abort(staging).catch(() => undefined);
      throw error;
    }
  }

  async tryHandleControl(
    meshName: string,
    record: ReceiveRecord,
    signal?: AbortSignal
  ): Promise<boolean> {
    if (record.parts.length !== 1) return false;
    const payload = record.parts[0]!.data();
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
    if (this.acceptControlResponse(request, record.sourceNodeRid)) return true;
    const response = await this.handleControl(meshName, request, record.sourceNodeRid, signal);
    if (record.sourceNodeRid === null) {
      throw new Error('Relocation control command has no authenticated source node.');
    }
    const submitted = this.requireMeshNode(meshName).sendToNode(
      record.sourceNodeRid,
      encodeServiceRelocationControlResponse(response)
    );
    if (submitted !== SubmitResult.Ok) {
      throw new Error('Relocation control reply was not accepted by RouteMesh.');
    }
    return true;
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
        for (const state of actorStates) {
          sessions.push({
            state,
            actor: state.actor!,
            prepared: await this.options.actorTransfer.prepareMaintenanceSession(
              state.actor!, state, captureSignal, false)
          });
        }
        spotCapture = await activation.captureRelocation(captureSignal);
        return {
          acceptedJournal: Buffer.alloc(0),
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
          await session.prepared.commit({
            routerChannelId: meshName,
            targetNodeRid: target.rid,
            spotId: activation.spotId,
            spotKind: kind === 'user_spot'
              ? ZLinkSpotKind.User
              : ZLinkSpotKind.Instance,
            authorityOwnerGeneration: committedAuthority.authorityOwnerGeneration
          } as never, {
            actorId: session.state.actorId,
            objectGeneration: actorAuthorities.get(session.state.actorId)!.objectGeneration,
            meshName,
            nodeRid: target.rid
          });
          activation.commitActorDeparture(session.state.actorId);
          await this.requireActorManager().completeRelocationSource(session.state.actorId);
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
        for (const session of [...sessions].reverse()) await session.prepared.rollback();
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
    const aggregateId = randomUUID();
    const units = [spotUnit, ...actorUnits];
    const preReservation = await this.preReserveRemote(
      meshName,
      target,
      spotAuthority,
      relocationManifest(aggregateId, 1n, units, memberships),
      units.length * DEFAULT_RELOCATION_PAYLOAD_ESTIMATE_BYTES,
      signal
    );
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
      const captureOwner = new ServiceRelocationObjectCaptureOwner(
        this.relocationLimits().maxConcurrentRelocationCaptures
      );
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
        sessions,
        preReservation.owner
      );
    } catch (error) {
      outcome = 'failed';
      await preReservation.owner.abortPreReservation().catch(() => undefined);
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
      await relocationStorePort(this.requireRelocationStore())
        .delete(preReservation.manifestRoot, signal).catch(() => undefined);
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
    }
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
      }
    );
    const membership: ServiceRelocationMembership = {
      actorKey: encodeAuthorityKey('actor', state.actorId).value,
      spotKey: encodeAuthorityKey('user_spot', String(membershipSpotId)).value,
      spotObjectGeneration:
        targetMembership?.spotObjectGeneration ?? target.lifecycleGeneration,
      membershipEpoch: state.spotMembershipEpoch > 0n ? state.spotMembershipEpoch : 1n
    };
    const aggregateId = randomUUID();
    let interruptionStartedAt: number | undefined;
    const measuredUnit: ServiceRelocationCaptureUnit = {
      ...unit,
      seal: async captureSignal => {
        interruptionStartedAt = performance.now();
        return unit.seal(captureSignal);
      }
    };
    const preReservation = await this.preReserveRemote(
      meshName,
      target,
      authority,
      relocationManifest(aggregateId, 1n, [measuredUnit], [membership]),
      DEFAULT_RELOCATION_PAYLOAD_ESTIMATE_BYTES,
      signal
    );
    let outcome = 'completed';
    try {
      const captured = await new ServiceRelocationObjectCaptureOwner(
        this.relocationLimits().maxConcurrentRelocationCaptures
      ).captureStandaloneActor(
        aggregateId, 1n, measuredUnit, membership, signal);
      await this.runCoordinator(
        meshName,
        target,
        authority,
        captured,
        new Map([[measuredUnit.authorityKey, authority]]),
        signal,
        sessions,
        preReservation.owner
      );
    } catch (error) {
      outcome = 'failed';
      await preReservation.owner.abortPreReservation().catch(() => undefined);
      throw error;
    } finally {
      await relocationStorePort(this.requireRelocationStore())
        .delete(preReservation.manifestRoot, signal).catch(() => undefined);
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
          acceptedJournal: Buffer.alloc(0),
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
    const aggregateId = randomUUID();
    const preReservation = await this.preReserveRemote(
      meshName,
      target,
      spotAuthority,
      relocationManifest(aggregateId, 1n, [spotUnit], []),
      DEFAULT_RELOCATION_PAYLOAD_ESTIMATE_BYTES,
      signal
    );
    let outcome = 'completed';
    try {
      const captured = await new ServiceRelocationObjectCaptureOwner(
        this.relocationLimits().maxConcurrentRelocationCaptures
      ).captureUserSpotAggregate(
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
        [],
        preReservation.owner
      );
    } catch (error) {
      outcome = 'failed';
      await preReservation.owner.abortPreReservation().catch(() => undefined);
      throw error;
    } finally {
      await relocationStorePort(this.requireRelocationStore())
        .delete(preReservation.manifestRoot, signal).catch(() => undefined);
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
    }
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
              state.actor!, state, signal)
          };
          sessions.push(ownSession);
        }
        const prepared = standalone ? ownSession : sessions.find(value => value.state === state);
        return {
          acceptedJournal: encodeActorSession(prepared?.prepared.target),
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
        await ownSession.prepared.commit({
          routerChannelId: meshName,
          targetNodeRid: target.rid,
          spotId: membershipSpotId,
          spotKind: targetMembership?.spotKind ?? ZLinkSpotKind.Entry,
          authorityOwnerGeneration: committedAuthority.authorityOwnerGeneration
        } as never, {
          actorId: state.actorId,
          objectGeneration: authority.objectGeneration,
          meshName,
          nodeRid: target.rid
        });
        await this.requireActorManager().completeRelocationSource(state.actorId);
      },
      abortSeal: async () => {
        if (standalone && ownSession !== undefined) await ownSession.prepared.rollback();
      }
    };
  }

  private async preReserveRemote(
    meshName: string,
    target: ZLinkMeshNodeDescriptor,
    primary: ZLinkAuthoritySnapshot,
    manifest: ServiceRelocationEnvelope,
    expectedBytes: number,
    signal?: AbortSignal
  ): Promise<RemotePreReservation> {
    const localStatus = this.requireMeshNode(meshName).status();
    const deadlineAtMs = Date.now() + 30_000;
    const owner = new RemoteRestoreOwner(
      meshName,
      String(target.rid),
      {
        ownerId: primary.ownerId,
        leaseGeneration: primary.ownerLeaseGeneration,
        nodeRid: String(localStatus.routingId),
        nodeGeneration: localStatus.lifecycleGeneration,
        expectedAuthorityStoreVersion: primary.storeVersion.value
      },
      {
        nodeRid: String(target.rid),
        nodeGeneration: target.lifecycleGeneration,
        ownerId: target.ownerId,
        ownerLeaseGeneration: target.leaseGeneration
      },
      target.applicationVersion,
      request => this.sendControl(
        meshName,
        target.rid,
        request,
        signal,
        deadlineAtMs
      )
    );
    const encoded = encodeServiceRelocationEnvelope(manifest);
    const stored = await relocationStorePort(this.requireRelocationStore())
      .put(encoded, 60_000, signal);
    try {
      await owner.preReserve(manifest, {
        reference: stored.reference,
        checksumCrc32c: stored.checksumCrc32c
      }, expectedBytes);
      return { owner, manifestRoot: stored.reference };
    } catch (error) {
      await relocationStorePort(this.requireRelocationStore())
        .delete(stored.reference, signal).catch(() => undefined);
      throw error;
    }
  }

  private async runCoordinator(
    meshName: string,
    target: ZLinkMeshNodeDescriptor,
    primary: ZLinkAuthoritySnapshot,
    captured: ServiceCapturedObjectRelocation,
    _authorities: ReadonlyMap<string, ZLinkAuthoritySnapshot>,
    signal?: AbortSignal,
    sessions: readonly SourceActorSession[] = [],
    preparedRemoteOwner?: RemoteRestoreOwner
  ): Promise<void> {
    const durable = new ServiceDurableRelocationRuntime(
      this.requireLocationStore(),
      relocationStorePort(this.requireRelocationStore()),
      this.codec
    );
    const localStatus = this.requireMeshNode(meshName).status();
    const controlDeadlineAtMs = Date.now() + 30_000;
    const remoteOwner = preparedRemoteOwner ?? new RemoteRestoreOwner(
      meshName,
      String(target.rid),
      {
        ownerId: primary.ownerId,
        leaseGeneration: primary.ownerLeaseGeneration,
        nodeRid: String(localStatus.routingId),
        nodeGeneration: localStatus.lifecycleGeneration,
        expectedAuthorityStoreVersion: primary.storeVersion.value
      },
      {
        nodeRid: String(target.rid),
        nodeGeneration: target.lifecycleGeneration,
        ownerId: target.ownerId,
        ownerLeaseGeneration: target.leaseGeneration
      },
      target.applicationVersion,
      request => this.sendControl(
        meshName,
        target.rid,
        request,
        signal,
        controlDeadlineAtMs
      )
    );
    const source = new ServiceCapturedRelocationSourceCompletion<RemoteStaging>(captured, {
      complete: async (_staging, authority, completionSignal) => {
        for (const session of sessions) {
          session.prepared.setReplayResults([]);
        }
        const pending = this.codec.read(authority.payload);
        if (pending === undefined) throw new Error('Pending relocation publication is missing.');
        const aggregate = captured.envelope.participants.length > 1;
        const retainedReference = aggregate && pending.phase === 'sourceCleanupPending'
          ? pending.reference
          : undefined;
        const completed = await durable.completeSourceCleanup(
          { value: primaryKey(captured.envelope) } as ZLinkAuthorityKey,
          authority,
          remoteOwner.successorProgress(captured.envelope),
          completionSignal,
          { retainPreviousRoot: retainedReference !== undefined }
        );
        await this.completeParticipantAuthorities(
          captured.envelope,
          primaryKey(captured.envelope),
          completed,
          completionSignal
        );
        if (retainedReference !== undefined) {
          await durable.deleteRetainedRoot(retainedReference, completionSignal);
        }
        return completed;
      }
    });
    const authorityCommit = this.authorityCommit(remoteOwner);
    const coordinator = new ServiceRelocationCoordinator(
      durable,
      remoteOwner,
      source,
      {
        relayCaptured: async (staging, _authority, relaySignal) => {
          remoteOwner.assertQueueReplayAcknowledged(captured.envelope);
          const previous = this.codec.read(_authority.payload);
          await remoteOwner.complete(staging, relaySignal);
          const delivered = await requireAuthority(
            this.requireLocationStore(),
            { value: primaryKey(captured.envelope) } as ZLinkAuthorityKey,
            relaySignal
          );
          await this.completeParticipantAuthorities(
            captured.envelope,
            primaryKey(captured.envelope),
            delivered,
            relaySignal
          );
          if (captured.envelope.participants.length > 1 && previous !== undefined) {
            await durable.deleteRetainedRoot(previous.reference, relaySignal);
          }
          return delivered;
        }
      },
      { replace: async () => undefined },
      {
        waitUntilReleasable: async (_staging, _authority, recoverySignal) =>
          this.releaseParticipantAuthorities(
            captured.envelope,
            primaryKey(captured.envelope),
            recoverySignal
          )
      },
      authorityCommit
    );
    let completed = false;
    let authorityCommitted = false;
    const relayAuthorityId = wireIdText(relocationWireId(captured.envelope.aggregateId));
    const relayAuthorityKey = primaryKey(captured.envelope);
    if (this.relocationAuthorityKeys.has(relayAuthorityId)) {
      throw new Error(`Relocation reply relay authority '${relayAuthorityId}' is already registered.`);
    }
    this.relocationAuthorityKeys.set(relayAuthorityId, relayAuthorityKey);
    let preparedStaging: RemoteStaging | undefined;
    const encoded = encodeServiceRelocationEnvelope(captured.envelope);
    const relocationStore = relocationStorePort(this.requireRelocationStore());
    const stored = await relocationStore.put(encoded, 60_000, signal);
    if (stored.checksumCrc32c !== crc32c(encoded)
      || stored.expiresAtMs <= stored.storeNowMs) {
      await relocationStore.delete(stored.reference, signal).catch(() => undefined);
      throw new Error('Relocation Store returned an invalid captured payload receipt.');
    }
    const publication: ServiceRelocationPublication = {
      phase: 'sourceCleanupPending',
      reference: stored.reference,
      checksumCrc32c: stored.checksumCrc32c,
      aggregateId: captured.envelope.aggregateId,
      aggregateGeneration: captured.envelope.aggregateGeneration,
      inventoryDigest: inventoryDigest(
        captured.envelope.participants,
        captured.envelope.memberships
      ),
      targetOwnerId: target.ownerId,
      targetOwnerLeaseGeneration: target.leaseGeneration
    };
    try {
      const key = {
        value: remoteOwner.primaryAuthorityKey = primaryKey(captured.envelope)
      } as ZLinkAuthorityKey;
      let committed;
      try {
        const staging = preparedStaging = await remoteOwner.prepare(
          captured.envelope,
          signal,
          publication
        );
        const pendingAuthority = {
          ...primary,
          payload: this.authorityPayloadForPublication(
            primary.payload,
            publication
          )
        };
        const authority = await authorityCommit.commit(
          key,
          pendingAuthority,
          { ownerId: target.ownerId, leaseGeneration: target.leaseGeneration },
          captured.envelope,
          undefined,
          signal
        );
        authorityCommitted = true;
        committed = await coordinator.resumeCommitted(key, { staging, authority }, signal);
      } catch (error) {
        if (!(error instanceof ServiceRelocationPostCommitError)) throw error;
        authorityCommitted = true;
        committed = await coordinator.resumeCommitted(key, {
          staging: error.staging as RemoteStaging,
          authority: error.authority
        }, signal);
      }
      if (committed.staging.id.length === 0) {
        throw new Error('Relocation target returned an empty staging identity.');
      }
      completed = true;
    } finally {
      if (!completed && !authorityCommitted) {
        if (preparedStaging !== undefined) {
          await remoteOwner.abort(preparedStaging).catch(() => undefined);
        }
        await relocationStore.delete(stored.reference).catch(() => undefined);
        await captured.abortSource().catch(() => undefined);
      }
      if (this.relocationAuthorityKeys.get(relayAuthorityId) === relayAuthorityKey) {
        this.relocationAuthorityKeys.delete(relayAuthorityId);
      }
    }
  }

  private authorityCommit(
    remoteOwner: RemoteRestoreOwner
  ): ServiceRelocationAuthorityCommit {
    return {
      commit: async (key, published, targetOwner, envelope, _fence, signal) => {
        let commitError: unknown;
        try {
          await remoteOwner.commitAuthority(envelope, signal);
        } catch (error) {
          commitError = error;
        }
        const current = await requireAuthority(this.requireLocationStore(), key, signal);
        const publication = this.codec.read(current.payload);
        if (current.ownerId !== targetOwner.ownerId
          || current.ownerLeaseGeneration !== targetOwner.leaseGeneration
          || current.authorityOwnerGeneration <= published.authorityOwnerGeneration
          || publication?.aggregateId !== envelope.aggregateId
          || publication.aggregateGeneration !== envelope.aggregateGeneration) {
          if (commitError !== undefined) throw commitError;
          throw new Error('Relocation target commit did not publish the exact owner fence.');
        }
        return current;
      }
    };
  }

  private async completeParticipantAuthorities(
    envelope: ServiceRelocationEnvelope,
    primaryKey: string,
    primary: ZLinkAuthoritySnapshot,
    signal?: AbortSignal
  ): Promise<void> {
    const completed = this.codec.read(primary.payload);
    if (completed === undefined) throw new Error('Completed relocation publication is missing.');
    for (const participant of envelope.participants) {
      if (participant.key === primaryKey) continue;
      const key = { value: participant.key } as ZLinkAuthorityKey;
      const current = await requireAuthority(this.requireLocationStore(), key, signal);
      const publication = this.codec.read(current.payload);
      if (publication === undefined) throw new Error(`Participant '${participant.key}' publication is missing.`);
      if (publication.reference === completed.reference
        && publication.aggregateGeneration === completed.aggregateGeneration) continue;
      if (publication.aggregateId !== completed.aggregateId
        || publication.aggregateGeneration + 1n !== completed.aggregateGeneration) {
        throw new Error(`Participant '${participant.key}' cleanup authority generation is stale.`);
      }
      const result = await this.requireLocationStore().compareExchangeAuthority(
        key,
        current.storeVersion,
        {
          kind: 'put',
          generationTransition: 'preserve',
          payload: this.codec.replace(current.payload, publication, completed)
        },
        signal
      );
      if (result.kind !== 'stored') {
        throw new Error(`Participant '${participant.key}' cleanup authority CAS failed.`);
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

  private async handleControl(
    meshName: string,
    request: ZLinkServiceRelocationControlRequest,
    sourceNodeRid: RoutingId | null,
    signal?: AbortSignal
  ): Promise<ZLinkServiceRelocationControlResponse> {
    this.pruneTargetTombstones();
    const stagingId = relocationStagingId(request);
    if (request.kind === 'prepare') {
      return this.handlePrepareControl(meshName, stagingId, request, sourceNodeRid, signal);
    }
    if (request.kind === 'ready') {
      return this.handleReadyControl(meshName, stagingId, request, sourceNodeRid, signal);
    }
    if (request.kind === 'data' && request.frozenRecord === undefined
      && (request.phase === 'prepared' || request.phase === 'committed'
        || request.phase === 'aborted')) {
      return this.handleStagingControl(meshName, stagingId, request, sourceNodeRid, signal);
    }
    const stage = this.targetStages.get(stagingId);
    if (stage === undefined) {
      const completed = this.completedTargets.get(stagingId);
      if (request.kind === 'complete' && request.sourceCleanupState === 'completed'
        && completed?.fingerprint === stringifyWire(request)
        && sourceNodeRid !== null
        && completed.authenticatedSourceNodeRid === String(sourceNodeRid)) {
        return completed.response;
      }
      throw new Error(`Relocation staging '${stagingId}' is missing.`);
    }
    if (!sameCoordinator(stage.coordinator, request.coordinator)) {
      throw new Error('Relocation coordinator fence changed.');
    }
    if (sourceNodeRid === null || String(sourceNodeRid) !== stage.offer.authenticatedSourceNodeRid) {
      throw new Error('Relocation control source does not match its prepare source.');
    }
    return this.handleMaterializedStageControl(meshName, stagingId, stage, request, signal);
  }

  private async handleMaterializedStageControl(
    meshName: string,
    stagingId: string,
    stage: LocalStage,
    request: ZLinkServiceRelocationControlRequest,
    signal?: AbortSignal
  ): Promise<ZLinkServiceRelocationControlResponse> {
    if (request.kind === 'complete' && request.sourceCleanupState === 'pending') {
      if (relocationPhaseRank(stage.phase) >= relocationPhaseRank('published')) {
        return { ...request, senderRole: 'target' };
      }
      requireRelocationPhase(stage, 'sealed');
      const authority = await requireAuthority(
        this.requireLocationStore(),
        { value: primaryKey(stage.staging.envelope) } as ZLinkAuthorityKey,
        signal
      );
      await stage.owner.publish(stage.staging, authority, signal);
      stage.phase = 'published';
      return { ...request, senderRole: 'target' };
    }
    if (request.kind === 'data') {
      const participantIndex = Number(request.participantId - 1n);
      if (!Number.isSafeInteger(participantIndex) || participantIndex < 0
        || participantIndex >= stage.staging.envelope.participants.length) {
        throw new Error('Relocation data participant is missing.');
      }
      const participant = stage.staging.envelope.participants[participantIndex]!;
      const queued = participant.queuedMessages.find(message => message.sequence === request.sequence);
      if (queued === undefined || request.frozenRecord === undefined) {
        throw new Error('Relocation data does not match a captured accepted record.');
      }
      const expectedRecord = canonicalQueuedFrozenRecord(
        participant,
        queued,
        stage.coordinator,
        stage.candidate
      );
      if (!request.frozenRecord.canonicalBytes.equals(expectedRecord.canonicalBytes)) {
        throw new Error('Relocation data frozen record changed after capture.');
      }
      if (relocationPhaseRank(stage.phase) < relocationPhaseRank('replayed')) {
        requireRelocationPhase(stage, 'prepared');
        await stage.owner.replayAcceptedJournal(stage.staging, signal);
        stage.phase = 'replayed';
      }
      return { kind: 'ack', relocation: request.relocation,
        targetAttemptGeneration: request.targetAttemptGeneration, coordinator: request.coordinator,
        senderRole: 'target', participantId: request.participantId,
        highWater: request.sequence };
    }
    if (request.kind === 'seal') {
      if (!request.response || request.senderRole !== 'source') {
        throw new Error('Relocation seal acknowledgement is invalid.');
      }
      if (relocationPhaseRank(stage.phase) < relocationPhaseRank('replayed')) {
        requireRelocationPhase(stage, 'prepared');
        await stage.owner.replayAcceptedJournal(stage.staging, signal);
        stage.phase = 'replayed';
      }
      if (relocationPhaseRank(stage.phase) < relocationPhaseRank('sealed')) {
        requireRelocationPhase(stage, 'replayed');
        stage.phase = 'sealed';
      }
      return { ...request, senderRole: 'target' };
    }
    if (request.kind === 'complete' && request.sourceCleanupState === 'completed') {
      return this.completeTargetStage(meshName, stagingId, stage, request, signal);
    }
    throw new Error(`Relocation command '${request.kind}' is invalid for target phase '${stage.phase}'.`);
  }

  /** Handles the authenticated, idempotent target preflight transition. */
  private async handlePrepareControl(
    meshName: string,
    stagingId: string,
    request: ServiceMaintenanceRelocationPrepare,
    sourceNodeRid: RoutingId | null,
    signal?: AbortSignal
  ): Promise<ZLinkServiceRelocationControlResponse> {
    if (sourceNodeRid === null || String(sourceNodeRid) !== request.sourceNodeRid) {
      throw new Error('Relocation prepare source node fence does not match the authenticated peer.');
    }
    const targetStatus = this.requireMeshNode(meshName).status();
    const targetOwner = this.options.currentOwner();
    const targetDescriptor = this.options.localDescriptor?.(meshName);
    if (targetOwner === undefined
      || String(targetStatus.routingId) !== request.candidate.nodeRid
      || targetStatus.lifecycleGeneration !== request.candidate.nodeGeneration
      || targetOwner.ownerId !== request.candidate.ownerId
      || targetOwner.leaseGeneration !== request.candidate.ownerLeaseGeneration
      || targetDescriptor !== undefined
        && targetDescriptor.applicationVersion !== request.applicationVersion) {
      throw new Error('Relocation prepare candidate fence does not match the target owner.');
    }
    if (request.root === undefined) throw new Error('Relocation prepare has no shared durable root.');
    const existing = this.targetStages.get(stagingId)?.offer
      ?? this.targetOffers.get(stagingId);
    if (existing !== undefined) {
      if (existing.authenticatedSourceNodeRid !== String(sourceNodeRid)) {
        throw new Error('Relocation prepare retry source node changed.');
      }
      if (request.round === 'preparedReplacement'
        && existing.prepare.round === 'initial') {
        const envelope = await this.readSharedEnvelope(request.root, signal);
        validateControlEnvelope(request, envelope);
        validateManifestReplacement(existing.envelope, envelope);
        const requiredMessages = request.participants.reduce(
          (sum, participant) => sum + participant.allowanceMessages, 0n);
        const requiredBytes = request.participants.reduce(
          (sum, participant) => sum + participant.allowanceBytes, 0n);
        if (requiredMessages > existing.offeredMessages
          || requiredBytes > existing.offeredBytes) {
          throw new Error('Captured relocation exceeds its preflight reservation.');
        }
        this.resizeTargetPermit(existing, requiredMessages, requiredBytes);
        if (existing.expiryTimer !== undefined) clearTimeout(existing.expiryTimer);
        const replacement: TargetRelocationOffer = {
          ...existing,
          prepare: request,
          prepareFingerprint: stringifyWire(request),
          envelope
        };
        this.targetOffers.set(stagingId, replacement);
        this.armTargetExpiry(stagingId, replacement);
        return relocationCapacityOffer(
          request,
          envelope,
          replacement.offeredMessages,
          replacement.offeredBytes
        );
      }
      validatePrepareOfferRetry(existing, request);
      return relocationCapacityOffer(
        request,
        existing.envelope,
        existing.offeredMessages,
        existing.offeredBytes
      );
    }
    const envelope = await this.readSharedEnvelope(request.root, signal);
    validateControlEnvelope(request, envelope);
    const raced = this.targetStages.get(stagingId)?.offer
      ?? this.targetOffers.get(stagingId);
    if (raced !== undefined) {
      if (raced.authenticatedSourceNodeRid !== String(sourceNodeRid)) {
        throw new Error('Relocation prepare retry source node changed.');
      }
      validatePrepareOfferRetry(raced, request);
      return relocationCapacityOffer(
        request,
        raced.envelope,
        raced.offeredMessages,
        raced.offeredBytes
      );
    }
    if (this.targetOffers.size + this.targetStages.size >= RELOCATION_TARGET_LIVE_LIMIT) {
      throw new Error('Relocation target live offer limit is exhausted.');
    }
    const offer = this.availableRelocationCapacity(envelope, request.requiredBytes);
    const targetOffer: TargetRelocationOffer = {
      prepare: request,
      prepareFingerprint: stringifyWire(request),
      authenticatedSourceNodeRid: String(sourceNodeRid),
      envelope,
      offeredMessages: offer.messages,
      offeredBytes: offer.bytes
    };
    this.armTargetExpiry(stagingId, targetOffer);
    this.targetOffers.set(stagingId, targetOffer);
    return relocationCapacityOffer(request, envelope, offer.messages, offer.bytes);
  }

  /** Reserves target capacity after the source accepts the preflight offer. */
  private async handleReadyControl(
    meshName: string,
    stagingId: string,
    request: ServiceMaintenanceRelocationReady,
    sourceNodeRid: RoutingId | null,
    signal?: AbortSignal
  ): Promise<ZLinkServiceRelocationControlResponse> {
    const stage = this.targetStages.get(stagingId);
    const offer = stage?.offer ?? this.targetOffers.get(stagingId);
    if (offer === undefined) {
      throw new Error(`Relocation offer '${stagingId}' is missing.`);
    }
    if (sourceNodeRid === null || String(sourceNodeRid) !== offer.authenticatedSourceNodeRid) {
      throw new Error('Relocation acceptance source does not match its prepare source.');
    }
    validateReadyAcceptance(offer, request);
    this.assertCurrentCandidate(meshName, offer.prepare.candidate);
    this.acquireTargetPermit(offer);
    try {
      if (offer.prepare.round === 'initial') {
        await this.ensureInitialTargetCapacityReservation(meshName, offer, signal);
      } else {
        await this.ensureTargetReservation(meshName, offer, signal);
      }
    } catch (error) {
      this.releaseTargetPermit(offer);
      throw error;
    }
    this.armTargetExpiry(stagingId, offer);
    return { kind: 'reserved', relocation: request.relocation,
      targetAttemptGeneration: request.targetAttemptGeneration, round: request.round,
      coordinator: request.coordinator, candidate: request.candidate,
      reservationGeneration: request.reservationGeneration, participants: request.participants };
  }

  /** Materializes a target offer and commits or aborts its staging barrier. */
  private async handleStagingControl(
    meshName: string,
    stagingId: string,
    request: ServiceMaintenanceRelocationControlData,
    sourceNodeRid: RoutingId | null,
    signal?: AbortSignal
  ): Promise<ZLinkServiceRelocationControlResponse> {
    if (request.senderRole !== 'source') {
      throw new Error('Relocation staging control source does not match the coordinator fence.');
    }
    const offer = this.targetStages.get(stagingId)?.offer ?? this.targetOffers.get(stagingId);
    if (offer === undefined) {
      const aborted = this.targetAborts.get(stagingId);
      if (request.phase === 'aborted'
        && aborted?.fingerprint === stringifyWire(request)
        && sourceNodeRid !== null
        && aborted.authenticatedSourceNodeRid === String(sourceNodeRid)) {
        return relocationControlAck(request);
      }
      throw new Error(`Relocation offer '${stagingId}' is missing.`);
    }
    if (sourceNodeRid === null || String(sourceNodeRid) !== offer.authenticatedSourceNodeRid) {
      throw new Error('Relocation staging control source does not match its prepare source.');
    }
    validateStagingControl(offer, request);
    if (request.phase === 'aborted') {
      await this.abortTargetOffer(stagingId, offer, signal);
      this.targetAborts.set(stagingId, {
        fingerprint: stringifyWire(request),
        authenticatedSourceNodeRid: offer.authenticatedSourceNodeRid,
        expiresAtMs: Date.now() + RELOCATION_TARGET_TOMBSTONE_TTL_MS
      });
      this.trimTargetTombstones(this.targetAborts);
      return relocationControlAck(request);
    }
    const reservation = await this.ensureTargetReservation(meshName, offer, signal);
    let materialized: LocalStage;
    try {
      offer.materialization ??= this.materializeTargetOffer(meshName, stagingId, offer, signal);
      materialized = await offer.materialization;
    } catch (error) {
      await this.abortTargetOffer(stagingId, offer, signal).catch(() => undefined);
      throw error;
    }
    if (request.phase === 'committed') {
      await this.commitTargetReservation(materialized, reservation, signal);
      offer.reservationCommitted = true;
      if (offer.expiryTimer !== undefined) clearTimeout(offer.expiryTimer);
    }
    return relocationControlAck(request);
  }

  private async completeTargetStage(
    meshName: string,
    stagingId: string,
    stage: LocalStage,
    request: ServiceMaintenanceRelocationComplete,
    signal?: AbortSignal
  ): Promise<ZLinkServiceRelocationControlResponse> {
    if (stage.phase !== 'open') {
      requireRelocationPhase(stage, 'sealed');
      await this.publishSessionRoutes(stage.staging);
      stage.phase = 'routed';
      let authority = await requireAuthority(
        this.requireLocationStore(),
        { value: primaryKey(stage.staging.envelope) } as ZLinkAuthorityKey,
        signal
      );
      const durable = new ServiceDurableRelocationRuntime(
        this.requireLocationStore(), relocationStorePort(this.requireRelocationStore()), this.codec);
      const pendingProgress = localSuccessorProgress(stage);
      if ([...pendingProgress.values()].some(value => value.terminalReplies.byteLength !== 0)) {
        const previous = this.codec.read(authority.payload);
        authority = await durable.advanceCompletedProgress(
          { value: primaryKey(stage.staging.envelope) } as ZLinkAuthorityKey,
          authority,
          pendingProgress,
          signal,
          { retainPreviousRoot: stage.staging.envelope.participants.length > 1 }
        );
        await this.completeParticipantAuthorities(
          stage.staging.envelope,
          primaryKey(stage.staging.envelope),
          authority,
          signal
        );
        if (stage.staging.envelope.participants.length > 1 && previous !== undefined) {
          await durable.deleteRetainedRoot(previous.reference, signal);
        }
      }
      if (!stage.terminalRelayed) {
        const completions = await this.readDurableTerminalCompletions(stage, authority, signal);
        await this.relayTerminalReplies(
          meshName,
          stage,
          this.targetReplyRelayCoordinator(meshName, stage, authority),
          completions,
          signal
        );
        stage.terminalRelayed = true;
      }
      const progress = localSuccessorProgress(stage);
      if ([...progress.values()].some(value => value.terminalReplies.byteLength !== 0)) {
        const previous = this.codec.read(authority.payload);
        authority = await durable.advanceCompletedProgress(
          { value: primaryKey(stage.staging.envelope) } as ZLinkAuthorityKey,
          authority,
          progress,
          signal,
          { retainPreviousRoot: stage.staging.envelope.participants.length > 1 }
        );
        await this.completeParticipantAuthorities(
          stage.staging.envelope,
          primaryKey(stage.staging.envelope),
          authority,
          signal
        );
        if (stage.staging.envelope.participants.length > 1 && previous !== undefined) {
          await durable.deleteRetainedRoot(previous.reference, signal);
        }
      }
      await stage.owner.normalize(stage.staging, authority, signal);
      stage.phase = 'normalized';
      await stage.owner.openAdmission(stage.staging, signal);
      stage.phase = 'open';
    }
    const response = { ...request, senderRole: 'target' as const };
    this.completedTargets.set(stagingId, {
      fingerprint: stringifyWire(request),
      authenticatedSourceNodeRid: stage.offer.authenticatedSourceNodeRid,
      response,
      expiresAtMs: Date.now() + RELOCATION_TARGET_TOMBSTONE_TTL_MS
    });
    this.trimTargetTombstones(this.completedTargets);
    if (stage.offer.expiryTimer !== undefined) clearTimeout(stage.offer.expiryTimer);
    this.releaseTargetPermit(stage.offer);
    this.targetStages.delete(stagingId);
    return response;
  }

  private async readSharedEnvelope(
    root: NonNullable<ServiceMaintenanceRelocationPrepare['root']>,
    signal?: AbortSignal
  ): Promise<ServiceRelocationEnvelope> {
    const read = await this.requireRelocationStore().read(
      relocationBlobReference(root.reference),
      signal
    );
    if (read.kind === 'missing' || crc32c(read.bytes) !== root.checksumCrc32c) {
      throw new Error('Relocation prepare references missing or corrupt shared durable data.');
    }
    return decodeServiceRelocationEnvelope(read.bytes);
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
    const publication = this.codec.read(authority.payload);
    if (authority.storeVersion.value !== relay.coordinator.expectedAuthorityStoreVersion
      || publication?.phase !== 'sourceCleanupCompleted'
      || !sameWireId(relocationWireId(publication.aggregateId), relay.relocation)) {
      throw new Error('Relocation reply relay coordinator authority fence is stale.');
    }
    const accepted = this.options.actorTransfer.relayCanonicalMaintenanceTerminal(
      wireIdText(relay.operation),
      relay.replyRouteId.toString(),
      handoffResultFromRelay(relay),
      String(sourceNodeRid)
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
      || requestSource.nodeRid !== String(localStatus.routingId)
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
    if (this.requireMeshNode(meshName).sendToNode(
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
          node.sendToNode(ackTargetNodeRid, encodeMaintenanceReplyRelay(request));
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
    if (owner === undefined
      || owner.ownerId !== stage.candidate.ownerId
      || owner.leaseGeneration !== stage.candidate.ownerLeaseGeneration
      || String(status.routingId) !== stage.candidate.nodeRid
      || status.lifecycleGeneration !== stage.candidate.nodeGeneration) {
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
    completions: readonly DurableRelocationTerminalCompletion[],
    signal?: AbortSignal
  ): Promise<void> {
    for (const completion of completions) {
      if (completion.delivery !== 'pending') continue;
      const hidden = stage.staging.hidden.get(completion.participantKey);
      if (hidden?.actor === undefined || hidden.targetAuthorityOwnerGeneration === undefined) {
        throw new Error('Durable relocation terminal participant is not a published Actor.');
      }
      const packet = hidden.replayPackets.find(value => value.index === completion.index);
      if (packet === undefined || !packet.returnResponse || packet.source === undefined
        || packet.messageFollowContext.operationId !== completion.operationId
        || !sameHandoffSource(packet.source, completion.source)) {
        throw new Error('Durable relocation terminal completion changed after replay.');
      }
      const request = maintenanceReplyRelay(
        stage,
        coordinator,
        completion.participantId,
        completion
      );
      const delivery = await this.sendReplyRelay(
        meshName,
        completion.source.nodeRid as RoutingId,
        request,
        handoffSourceFence(completion.source),
        signal
      );
      hidden.terminalDeliveries.set(completion.index, delivery);
    }
  }

  private async readDurableTerminalCompletions(
    stage: LocalStage,
    authority: ZLinkAuthoritySnapshot,
    signal?: AbortSignal
  ): Promise<readonly DurableRelocationTerminalCompletion[]> {
    const publication = this.codec.read(authority.payload);
    if (publication?.phase !== 'sourceCleanupCompleted') {
      throw new Error('Relocation terminal completion requires a completed durable root.');
    }
    const read = await this.requireRelocationStore().read(
      relocationBlobReference(publication.reference),
      signal
    );
    if (read.kind !== 'found' || crc32c(read.bytes) !== publication.checksumCrc32c) {
      throw new Error('Relocation terminal completion durable root is missing or corrupt.');
    }
    const envelope = decodeServiceRelocationEnvelope(read.bytes);
    if (envelope.aggregateId !== stage.staging.envelope.aggregateId) {
      throw new Error('Relocation terminal completion durable identity changed.');
    }
    if (envelope.participants.length !== stage.staging.envelope.participants.length) {
      throw new Error('Relocation terminal completion participant count changed.');
    }
    const completions: DurableRelocationTerminalCompletion[] = [];
    for (const [index, participant] of envelope.participants.entries()) {
      const expected = stage.staging.envelope.participants[index]!;
      if (expected.key !== participant.key) {
        throw new Error('Relocation terminal completion participant order changed.');
      }
      const values = decodeTerminalCompletions(participant.terminalReplies);
      if (values.filter(value => value.delivery === 'pending').length
        !== participant.pendingRelayCount) {
        throw new Error('Relocation terminal completion pending relay count changed.');
      }
      for (const value of values) {
        completions.push({ ...value, participantKey: participant.key,
          participantId: BigInt(index + 1) });
      }
    }
    return completions;
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
      const send = () => {
        if (signal?.aborted === true) {
          finish(() => reject(signal.reason));
          return;
        }
        if (Date.now() >= deadlineAtMs) {
          finish(() => reject(new Error(`Relocation control ACK '${key}' timed out.`)));
          return;
        }
        node.sendToNode(targetNodeRid, encodeServiceRelocationControlRequest(request));
      };
      const timer = setInterval(send, 250);
      const finish = (action: () => void) => {
        clearInterval(timer);
        this.pendingControls.delete(key);
        action();
      };
      this.pendingControls.set(key, {
        targetNodeRid: String(targetNodeRid), request,
        resolve: response => finish(() => resolve(response)),
        reject: error => finish(() => reject(error)), timer
      });
      send();
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

  private pruneTargetTombstones(): void {
    const now = Date.now();
    for (const [key, value] of this.targetAborts) {
      if (value.expiresAtMs <= now) this.targetAborts.delete(key);
    }
    for (const [key, value] of this.completedTargets) {
      if (value.expiresAtMs <= now) this.completedTargets.delete(key);
    }
  }

  private trimTargetTombstones<T extends { readonly expiresAtMs: number }>(
    values: Map<string, T>
  ): void {
    while (values.size > RELOCATION_TARGET_TOMBSTONE_LIMIT) {
      const oldest = values.keys().next().value;
      if (oldest === undefined) return;
      values.delete(oldest);
    }
  }

  private availableRelocationCapacity(
    envelope?: ServiceRelocationEnvelope,
    requestedBytes = 0n
  ): { readonly messages: bigint; readonly bytes: bigint } {
    const limits = this.relocationLimits();
    const remainingMessages = BigInt(limits.maxActiveInboundRelocations)
      * RELOCATION_MESSAGE_ESTIMATE_PER_UNIT - this.reservedTargetMessages;
    const messages = remainingMessages < RELOCATION_MESSAGE_ESTIMATE_PER_UNIT
      ? remainingMessages
      : RELOCATION_MESSAGE_ESTIMATE_PER_UNIT;
    const remainingBytes =
      BigInt(limits.maxRelocationPayloadInFlightBytes) - this.reservedTargetBytes;
    const oversizedExclusive = envelope !== undefined
      && isUserSpotAggregate(envelope)
      && requestedBytes > BigInt(limits.maxRelocationPayloadInFlightBytes)
      && this.activeTargetRelocations === 0
      && this.reservedTargetBytes === 0n;
    const bytes = oversizedExclusive ? requestedBytes : remainingBytes;
    if (messages <= 0n || bytes <= 0n
      || this.activeTargetRelocations >= limits.maxActiveInboundRelocations) {
      throw new Error('Relocation target replay capacity is exhausted.');
    }
    return { messages, bytes };
  }

  private acquireTargetPermit(offer: TargetRelocationOffer): void {
    const existing = offer.permit;
    if (existing !== undefined && !existing.released) return;
    const messages = existing?.messages ?? offer.prepare.participants.reduce(
      (sum, participant) => sum + participant.allowanceMessages,
      0n
    );
    const bytes = existing?.bytes ?? offer.prepare.participants.reduce(
      (sum, participant) => sum + participant.allowanceBytes,
      0n
    );
    const limits = this.relocationLimits();
    const oversizedUserSpot = isUserSpotAggregate(offer.envelope)
      && bytes > BigInt(limits.maxRelocationPayloadInFlightBytes)
      && this.activeTargetRelocations === 0
      && this.reservedTargetBytes === 0n;
    const available = this.availableRelocationCapacity(offer.envelope, bytes);
    if (messages > available.messages || (bytes > available.bytes && !oversizedUserSpot)) {
      throw new Error('Relocation target replay capacity is exhausted.');
    }
    if (existing === undefined) {
      offer.permit = { messages, bytes, active: true, released: false };
    } else {
      existing.released = false;
    }
    this.reservedTargetMessages += messages;
    this.reservedTargetBytes += bytes;
    this.activeTargetRelocations++;
  }

  private releaseTargetPermit(offer: TargetRelocationOffer): void {
    const permit = offer.permit;
    if (permit === undefined || permit.released) return;
    permit.released = true;
    this.reservedTargetMessages -= permit.messages;
    this.reservedTargetBytes -= permit.bytes;
    if (permit.active) this.activeTargetRelocations--;
  }

  private resizeTargetPermit(
    offer: TargetRelocationOffer,
    messages: bigint,
    bytes: bigint
  ): void {
    const permit = offer.permit;
    if (permit === undefined || permit.released) {
      throw new Error('Prepared relocation replacement has no active target permit.');
    }
    const limits = this.relocationLimits();
    const nextMessages = this.reservedTargetMessages - permit.messages + messages;
    const nextBytes = this.reservedTargetBytes - permit.bytes + bytes;
    const maxMessages = BigInt(limits.maxActiveInboundRelocations)
      * RELOCATION_MESSAGE_ESTIMATE_PER_UNIT;
    const oversizedUserSpot = isUserSpotAggregate(offer.envelope)
      && bytes > BigInt(limits.maxRelocationPayloadInFlightBytes)
      && this.activeTargetRelocations === 1
      && this.reservedTargetBytes === permit.bytes;
    if (nextMessages > maxMessages
      || (nextBytes > BigInt(limits.maxRelocationPayloadInFlightBytes)
        && !oversizedUserSpot)) {
      throw new Error('Captured relocation exceeds available target permits.');
    }
    this.reservedTargetMessages = nextMessages;
    this.reservedTargetBytes = nextBytes;
    permit.messages = messages;
    permit.bytes = bytes;
  }

  private assertCurrentCandidate(
    meshName: string,
    candidate: ServiceWireRelocationCandidate
  ): void {
    const status = this.requireMeshNode(meshName).status();
    const owner = this.options.currentOwner();
    if (owner === undefined
      || String(status.routingId) !== candidate.nodeRid
      || status.lifecycleGeneration !== candidate.nodeGeneration
      || owner.ownerId !== candidate.ownerId
      || owner.leaseGeneration !== candidate.ownerLeaseGeneration) {
      throw new Error('Relocation candidate fence does not match the current target owner.');
    }
  }

  private async ensureTargetReservation(
    meshName: string,
    offer: TargetRelocationOffer,
    signal?: AbortSignal
  ): Promise<TargetRelocationReservation> {
    offer.reservation ??= isUserSpotAggregate(offer.envelope)
      ? this.reserveTargetAggregateOffer(meshName, offer, signal)
      : offer.capacityReservation
        ?? this.reserveInitialTargetCapacity(meshName, offer, signal);
    return await offer.reservation;
  }

  private async ensureInitialTargetCapacityReservation(
    meshName: string,
    offer: TargetRelocationOffer,
    signal?: AbortSignal
  ): Promise<Extract<TargetRelocationReservation, { readonly kind: 'single' }>> {
    offer.capacityReservation ??= this.reserveInitialTargetCapacity(meshName, offer, signal);
    return await offer.capacityReservation;
  }

  private async reserveInitialTargetCapacity(
    meshName: string,
    offer: TargetRelocationOffer,
    signal?: AbortSignal
  ): Promise<Extract<TargetRelocationReservation, { readonly kind: 'single' }>> {
    this.assertCurrentCandidate(meshName, offer.prepare.candidate);
    const authorities = await this.readTargetParticipantAuthorities(offer, false, signal);
    const participant = offer.envelope.participants[0]!;
    const expected = authorities.get(participant.key)!;
    const capacity = isUserSpotAggregate(offer.envelope)
      ? [...authorities.values()].reduce(
          (sum, value) => addCapacity(sum, value.allocation.capacity),
          { actors: 0, spots: 0 }
        )
      : expected.allocation.capacity;
    const request = {
      reservationId: offer.envelope.aggregateId,
      authorityKey: { value: participant.key } as ZLinkAuthorityKey,
      expectedStoreVersion: expected.storeVersion,
      objectKind: expected.allocation.objectKind,
      stableType: expected.allocation.stableType,
      sourceDescriptor: expected.allocation.descriptor,
      sourceNodeLifecycleGeneration: expected.allocation.descriptorLifecycleGeneration,
      sourceOwner: {
        ownerId: expected.ownerId,
        leaseGeneration: expected.ownerLeaseGeneration
      },
      targetDescriptor: { meshName, rid: offer.prepare.candidate.nodeRid as RoutingId },
      targetNodeLifecycleGeneration: offer.prepare.candidate.nodeGeneration,
      targetOwner: {
        ownerId: offer.prepare.candidate.ownerId,
        leaseGeneration: offer.prepare.candidate.ownerLeaseGeneration
      },
      capacity
    };
    const reserve = async (reconcileSignal?: AbortSignal) =>
      await this.reserveExactSingleTarget(request, reconcileSignal);
    offer.reconcileReservation = reserve;
    return await reserve(signal);
  }

  private async reserveTargetAggregateOffer(
    meshName: string,
    offer: TargetRelocationOffer,
    signal?: AbortSignal
  ): Promise<TargetRelocationReservation> {
    this.assertCurrentCandidate(meshName, offer.prepare.candidate);
    await this.ensureInitialTargetCapacityReservation(meshName, offer, signal);
    const authorities = await this.readTargetParticipantAuthorities(offer, true, signal);
    const committer = new ServiceRelocationAggregateCommitter(this.requireLocationStore());
    const plan = this.targetAggregatePlan(meshName, offer, authorities);
    offer.reconcileReservation = async reconcileSignal => ({
      kind: 'aggregate',
      prepared: await committer.prepare(plan, reconcileSignal)
    });
    return await offer.reconcileReservation(signal);
  }

  private async reserveExactSingleTarget(
    request: Parameters<ZLinkLocationStore['reserveRelocationCapacity']>[0],
    signal?: AbortSignal
  ): Promise<Extract<TargetRelocationReservation, { readonly kind: 'single' }>> {
    const result = await this.requireLocationStore().reserveRelocationCapacity(request, signal);
    if (result.kind !== 'reserved' && result.kind !== 'alreadyReserved') {
      throw new TargetReservationRejectedError(
        'Relocation target capacity reservation failed with ' + result.kind + '.'
      );
    }
    return { kind: 'single', fence: result.fence };
  }

  private armTargetExpiry(stagingId: string, offer: TargetRelocationOffer): void {
    if (offer.expiryTimer !== undefined) clearTimeout(offer.expiryTimer);
    offer.expiryTimer = setTimeout(() => {
      void this.expireTargetOffer(stagingId, offer);
    }, 30_000);
    offer.expiryTimer.unref();
  }

  private async expireTargetOffer(stagingId: string, offer: TargetRelocationOffer): Promise<void> {
    if (this.targetOffers.get(stagingId) !== offer
      && this.targetStages.get(stagingId)?.offer !== offer) return;
    if (offer.reservation === undefined && offer.capacityReservation === undefined) {
      this.targetOffers.delete(stagingId);
      this.releaseTargetPermit(offer);
      return;
    }
    const authority = await this.requireLocationStore().readAuthority(
      { value: primaryKey(offer.envelope) } as ZLinkAuthorityKey
    ).catch(() => undefined);
    const publication = authority?.kind === 'snapshot'
      ? this.codec.read(authority.payload)
      : undefined;
    if (authority?.kind === 'snapshot'
      && authority.ownerId === offer.prepare.candidate.ownerId
      && authority.ownerLeaseGeneration === offer.prepare.candidate.ownerLeaseGeneration
      && publication?.aggregateId === offer.envelope.aggregateId
      && publication.aggregateGeneration === offer.envelope.aggregateGeneration) {
      offer.reservationCommitted = true;
      this.releaseTargetPermit(offer);
      return;
    }
    await this.abortTargetOffer(stagingId, offer).catch(() => undefined);
  }

  private async readTargetParticipantAuthorities(
    offer: TargetRelocationOffer,
    captured: boolean,
    signal?: AbortSignal
  ): Promise<ReadonlyMap<string, ZLinkAuthoritySnapshot>> {
    const authorities = new Map<string, ZLinkAuthoritySnapshot>();
    for (const participant of offer.envelope.participants) {
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
    if (captured) {
      const publication = this.relocationPublication(offer);
      if (publication.reference !== offer.prepare.root?.reference
        || publication.checksumCrc32c !== offer.prepare.root.checksumCrc32c) {
        throw new Error('Relocation target reservation has no exact captured root.');
      }
    }
    return authorities;
  }

  private targetAggregatePlan(
    meshName: string,
    offer: TargetRelocationOffer,
    authorities: ReadonlyMap<string, ZLinkAuthoritySnapshot>
  ): ServiceRelocationAggregatePlan {
    const publication = this.relocationPublication(offer);
    const participants = offer.envelope.participants.map(participant => {
      const expected = authorities.get(participant.key)!;
      return {
        key: { value: participant.key } as ZLinkAuthorityKey,
        expected,
        ownerTransition: 'newOwner' as const,
        authorityPayload: this.authorityPayloadForPublication(
          expected.payload,
          publication
        ),
        membershipMutation: encodeMembershipMutation(offer.envelope.memberships, participant.key)
      };
    });
    return {
      envelope: offer.envelope,
      participants,
      targetDescriptor: { meshName, rid: offer.prepare.candidate.nodeRid as RoutingId },
      targetDescriptorLifecycleGeneration: offer.prepare.candidate.nodeGeneration,
      capacity: participants.reduce((sum, participant) =>
        addCapacity(sum, participant.expected.allocation.capacity), { actors: 0, spots: 0 }),
      targetOwner: {
        ownerId: offer.prepare.candidate.ownerId,
        leaseGeneration: offer.prepare.candidate.ownerLeaseGeneration
      }
    };
  }

  private relocationPublication(offer: TargetRelocationOffer): ServiceRelocationPublication {
    const root = offer.prepare.root;
    if (root === undefined) throw new Error('Relocation target offer has no durable root.');
    return {
      phase: 'sourceCleanupPending',
      reference: root.reference,
      checksumCrc32c: root.checksumCrc32c,
      aggregateId: offer.envelope.aggregateId,
      aggregateGeneration: offer.envelope.aggregateGeneration,
      inventoryDigest: inventoryDigest(
        offer.envelope.participants,
        offer.envelope.memberships
      ),
      targetOwnerId: offer.prepare.candidate.ownerId,
      targetOwnerLeaseGeneration:
        offer.prepare.candidate.ownerLeaseGeneration
    };
  }

  private authorityPayloadForPublication(
    payload: Uint8Array,
    publication: ServiceRelocationPublication
  ): Uint8Array {
    const existing = this.codec.read(payload);
    if (existing === undefined) return this.codec.publish(payload, publication);
    if (existing.phase !== publication.phase
      || existing.reference !== publication.reference
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

  private async abortTargetOffer(
    stagingId: string,
    offer: TargetRelocationOffer,
    signal?: AbortSignal
  ): Promise<void> {
    if (offer.reservationCommitted === true) {
      throw new Error('Committed relocation target reservation cannot be aborted.');
    }
    let durableAbortCompleted = false;
    try {
      const stage = this.targetStages.get(stagingId)
        ?? (offer.materialization === undefined
          ? undefined
          : await offer.materialization.catch(() => undefined));
      if (stage !== undefined) await stage.owner.abort(stage.staging);
      const reservation = await this.reconcileTargetReservationForAbort(offer, signal);
      if (reservation?.kind === 'aggregate') {
        await new ServiceRelocationAggregateCommitter(this.requireLocationStore())
          .abort(reservation.prepared, signal);
      } else if (reservation?.kind === 'single') {
        const result = await this.requireLocationStore().abortRelocationCapacity(
          reservation.fence,
          signal
        );
        if (result !== 'aborted' && result !== 'alreadyAborted') {
          throw new Error('Relocation target capacity abort failed with ' + result + '.');
        }
      }
      durableAbortCompleted = true;
    } finally {
      if (durableAbortCompleted) {
        if (offer.expiryTimer !== undefined) clearTimeout(offer.expiryTimer);
        this.releaseTargetPermit(offer);
        this.targetStages.delete(stagingId);
        this.targetOffers.delete(stagingId);
      } else if (this.targetOffers.get(stagingId) === offer
        || this.targetStages.get(stagingId)?.offer === offer) {
        this.armTargetExpiry(stagingId, offer);
      }
    }
  }

  private async reconcileTargetReservationForAbort(
    offer: TargetRelocationOffer,
    signal?: AbortSignal
  ): Promise<TargetRelocationReservation | undefined> {
    const pending = offer.reservation ?? offer.capacityReservation;
    if (pending === undefined) return undefined;
    try {
      return await pending;
    } catch (error) {
      if (offer.capacityReservation !== undefined) {
        return await offer.capacityReservation;
      }
      if (error instanceof TargetReservationRejectedError) return undefined;
      if (offer.reconcileReservation === undefined) throw error;
      return await offer.reconcileReservation(signal);
    }
  }

  private async commitTargetReservation(
    stage: LocalStage,
    reservation: TargetRelocationReservation,
    signal?: AbortSignal
  ): Promise<void> {
    if (reservation.kind === 'aggregate') {
      await new ServiceRelocationAggregateCommitter(this.requireLocationStore())
        .commit(reservation.prepared, signal);
      stage.offer.reservationCommitted = true;
      return;
    }
    const key = stage.staging.primaryAuthorityKey;
    const current = await requireAuthority(this.requireLocationStore(), key, signal);
    const publication = this.codec.read(current.payload);
    if (current.ownerId === stage.candidate.ownerId
      && current.ownerLeaseGeneration === stage.candidate.ownerLeaseGeneration
      && publication?.aggregateId === stage.staging.envelope.aggregateId
      && publication.aggregateGeneration === stage.staging.envelope.aggregateGeneration) {
      return;
    }
    const durable = new ServiceDurableRelocationRuntime(
      this.requireLocationStore(),
      relocationStorePort(this.requireRelocationStore()),
      this.codec
    );
    const published = {
      ...current,
      payload: this.authorityPayloadForPublication(
        current.payload,
        this.relocationPublication(stage.offer)
      )
    };
    await durable.commitOwner(
      key,
      published,
      {
        ownerId: stage.candidate.ownerId,
        leaseGeneration: stage.candidate.ownerLeaseGeneration
      },
      reservation.fence,
      signal
    );
    stage.offer.reservationCommitted = true;
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
      value => ({ value } as ZLinkAuthorityKey),
      this.relocationLimits().maxConcurrentRelocationRestores
    );
    const staging = await owner.prepare(offer.envelope, signal);
    if (staging.id !== `${offer.envelope.aggregateId}:${offer.envelope.aggregateGeneration}`) {
      throw new Error('Relocation materialization returned a different staging identity.');
    }
    const stage: LocalStage = {
      offer,
      owner,
      staging,
      coordinator: offer.prepare.coordinator,
      candidate: offer.prepare.candidate,
      phase: 'prepared',
      terminalRelayed: false
    };
    this.targetStages.set(stagingId, stage);
    this.targetOffers.delete(stagingId);
    return stage;
  }

  private spotKind(
    meshName: string,
    activation: ZLinkSpotActivation
  ): 'user_spot' | 'instance_spot' | undefined {
    const node = this.options.registration.spotNodes.get(meshName);
    if (Object.values(node?.spotFactoryRegistrations ?? {})
      .some(value => value.implementation === activation.spotType)) return 'user_spot';
    if (Object.values(node?.instanceSpotFactoryRegistrations ?? {})
      .some(value => value.implementation === activation.spotType)) return 'instance_spot';
    return undefined;
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
      throw new Error('Relocation application state exceeds the 64 MiB participant limit.');
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

class RemoteRestoreOwner implements ServiceRelocationRestoreOwner<RemoteStaging> {
  primaryAuthorityKey?: string;
  private readonly highWater = new Map<bigint, bigint>();
  private staging?: RemoteStaging;
  private preReserved?: RemoteStaging;

  constructor(
    private readonly meshName: string,
    private readonly targetNodeRid: string,
    private readonly coordinator: ServiceWireRelocationCoordinatorFence,
    private readonly candidate: ServiceWireRelocationCandidate,
    private readonly applicationVersion: bigint,
    private readonly request: (
      request: ZLinkServiceRelocationControlRequest
    ) => Promise<ReturnType<typeof decodeServiceRelocationControlResponse>>
  ) {}

  async preReserve(
    manifest: ServiceRelocationEnvelope,
    root: NonNullable<ServiceMaintenanceRelocationPrepare['root']>,
    expectedBytes: number
  ): Promise<void> {
    if (this.preReserved !== undefined) {
      throw new Error('Relocation target capacity is already reserved.');
    }
    const canonicalManifest = canonicalRelocationEnvelope(manifest);
    const relocation = relocationWireId(canonicalManifest.aggregateId);
    const object = relocationObject(canonicalManifest);
    const participants = relocationEstimatedParticipants(
      canonicalManifest,
      expectedBytes
    );
    const prepare: ServiceMaintenanceRelocationPrepare = {
      kind: 'prepare',
      relocation,
      targetAttemptGeneration: canonicalManifest.aggregateGeneration,
      round: 'initial',
      coordinator: this.coordinator,
      candidate: this.candidate,
      initiatorRole: 'source',
      object,
      sourceNodeRid: this.coordinator.nodeRid,
      sourceNodeGeneration: this.coordinator.nodeGeneration,
      requiredMessages: participants.reduce(
        (sum, participant) => sum + participant.allowanceMessages, 0n),
      requiredBytes: BigInt(expectedBytes),
      participants,
      root,
      applicationVersion: this.applicationVersion
    };
    const offered = await this.request(prepare);
    if (offered.kind !== 'ready' || offered.role !== 'target'
      || offered.offeredMessages < prepare.requiredMessages
      || offered.offeredBytes < prepare.requiredBytes) {
      throw new Error('Relocation target did not return the preflight capacity offer.');
    }
    const reserved = await this.request({
      ...offered,
      role: 'source',
      offeredMessages: 0n,
      offeredBytes: 0n,
      participants
    });
    if (reserved.kind !== 'reserved'
      || reserved.reservationGeneration !== offered.reservationGeneration) {
      throw new Error('Relocation target did not reserve preflight capacity.');
    }
    this.preReserved = {
      id: `${canonicalManifest.aggregateId}:${canonicalManifest.aggregateGeneration}`,
      meshName: this.meshName,
      targetNodeRid: this.targetNodeRid,
      envelope: canonicalManifest,
      relocation,
      targetAttemptGeneration: canonicalManifest.aggregateGeneration,
      candidate: this.candidate,
      object,
      participants,
      root
    };
  }

  async abortPreReservation(): Promise<void> {
    const reserved = this.preReserved;
    if (reserved === undefined || this.staging !== undefined) return;
    await this.request(this.stagingControl(reserved, 'aborted'));
    this.preReserved = undefined;
  }

  async prepare(
    envelope: ServiceRelocationEnvelope,
    _signal?: AbortSignal,
    publication?: ServiceRelocationPublication
  ): Promise<RemoteStaging> {
    if (publication === undefined) throw new Error('Relocation prepare has no durable publication.');
    const canonicalEnvelope = canonicalRelocationEnvelope(envelope);
    const id = `${canonicalEnvelope.aggregateId}:${canonicalEnvelope.aggregateGeneration}`;
    const relocation = relocationWireId(canonicalEnvelope.aggregateId);
    const object = relocationObject(canonicalEnvelope);
    const participants = relocationParticipants(
      canonicalEnvelope,
      this.coordinator,
      this.candidate
    );
    const root = { reference: publication.reference, checksumCrc32c: publication.checksumCrc32c };
    const requiredMessages = participants.reduce((sum, value) => sum + value.allowanceMessages, 0n);
    const requiredBytes = participants.reduce((sum, value) => sum + value.allowanceBytes, 0n);
    const prepare: ServiceMaintenanceRelocationPrepare = {
      kind: 'prepare', relocation,
      targetAttemptGeneration: canonicalEnvelope.aggregateGeneration,
      round: this.preReserved === undefined ? 'initial' : 'preparedReplacement',
      coordinator: this.coordinator, candidate: this.candidate,
      initiatorRole: 'source', object, sourceNodeRid: this.coordinator.nodeRid,
      sourceNodeGeneration: this.coordinator.nodeGeneration, requiredMessages, requiredBytes,
      participants, root, applicationVersion: this.applicationVersion
    };
    const offered = await this.request(prepare);
    if (offered.kind !== 'ready' || offered.role !== 'target'
      || offered.offeredMessages < requiredMessages
      || offered.offeredBytes < requiredBytes) {
      throw new Error('Relocation target did not return a capacity offer.');
    }
    const reserved = await this.request({
      ...offered,
      role: 'source',
      offeredMessages: 0n,
      offeredBytes: 0n,
      participants
    });
    if (reserved.kind !== 'reserved'
      || reserved.reservationGeneration !== offered.reservationGeneration) {
      throw new Error('Relocation target did not acknowledge the reservation.');
    }
    const staging = {
      id,
      meshName: this.meshName,
      targetNodeRid: this.targetNodeRid,
      envelope: canonicalEnvelope,
      relocation,
      targetAttemptGeneration: canonicalEnvelope.aggregateGeneration,
      candidate: this.candidate, object, participants, root };
    try {
      const prepared = await this.request(this.stagingControl(staging, 'prepared'));
      if (prepared.kind !== 'ack' || prepared.participantId !== 1n || prepared.highWater !== 1n) {
        throw new Error('Relocation target did not acknowledge prepared staging.');
      }
      this.staging = staging;
      this.preReserved = undefined;
      return staging;
    } catch (error) {
      await this.request(this.stagingControl(staging, 'aborted')).catch(() => undefined);
      throw error;
    }
  }

  async publish(staging: RemoteStaging): Promise<void> {
    const response = await this.request(this.completion(staging, 'pending'));
    if (response.kind !== 'complete' || response.sourceCleanupState !== 'pending') {
      throw new Error('Relocation target did not publish the reserved owner.');
    }
  }

  async replayAcceptedJournal(staging: RemoteStaging): Promise<void> {
    for (const [index, participant] of staging.participants.entries()) {
      const captured = staging.envelope.participants[index]!;
      let highWater = captured.replayCursor;
      for (const message of captured.queuedMessages) {
        const frozenRecord = canonicalQueuedFrozenRecord(
          captured,
          message,
          this.coordinator,
          staging.candidate
        );
        const response = await this.request({ kind: 'data', relocation: staging.relocation,
          targetAttemptGeneration: staging.targetAttemptGeneration, coordinator: this.coordinator,
          senderRole: 'source', participantId: participant.participantId,
          sequence: message.sequence, frozenRecord });
        if (response.kind !== 'ack' || response.participantId !== participant.participantId
          || response.highWater !== message.sequence) {
          throw new Error('Relocation target returned an invalid replay high-water ACK.');
        }
        highWater = response.highWater;
      }
      this.highWater.set(participant.participantId, highWater);
    }
    const response = await this.request({ kind: 'seal', relocation: staging.relocation,
      targetAttemptGeneration: staging.targetAttemptGeneration, coordinator: this.coordinator,
      senderRole: 'source', response: true,
      participants: staging.participants.map(value => ({ participantId: value.participantId,
        highWater: this.highWater.get(value.participantId) ?? 0n })) });
    if (response.kind !== 'seal' || response.senderRole !== 'target') {
      throw new Error('Relocation target did not close the replay seal.');
    }
  }

  async normalize(): Promise<void> {}

  async openAdmission(): Promise<void> {}

  async abort(staging: RemoteStaging): Promise<void> {
    const response = await this.request(this.stagingControl(staging, 'aborted'));
    if (response.kind !== 'ack' || response.participantId !== 1n || response.highWater !== 1n) {
      throw new Error('Relocation target did not acknowledge precommit abort cleanup.');
    }
    if (this.staging?.id === staging.id) this.staging = undefined;
  }

  async commitAuthority(
    envelope: ServiceRelocationEnvelope,
    _signal?: AbortSignal
  ): Promise<void> {
    const staging = this.staging;
    if (staging === undefined || staging.envelope.aggregateId !== envelope.aggregateId
      || staging.envelope.aggregateGeneration !== envelope.aggregateGeneration) {
      throw new Error('Relocation target commit has no exact prepared staging.');
    }
    const response = await this.request(this.stagingControl(staging, 'committed'));
    if (response.kind !== 'ack' || response.participantId !== 1n || response.highWater !== 1n) {
      throw new Error('Relocation target did not acknowledge owner commit.');
    }
  }

  async complete(
    staging: RemoteStaging,
    _signal?: AbortSignal
  ): Promise<void> {
    const response = await this.request(this.completion(staging, 'completed'));
    if (response.kind !== 'complete' || response.sourceCleanupState !== 'completed') {
      throw new Error('Relocation target did not acknowledge source cleanup completion.');
    }
  }

  assertQueueReplayAcknowledged(envelope: ServiceRelocationEnvelope): void {
    const participants = this.staging?.envelope.participants
      ?? canonicalRelocationEnvelope(envelope).participants;
    for (const [index, participant] of participants.entries()) {
      const expected = participant.queuedMessages.at(-1)?.sequence ?? participant.replayCursor;
      if (this.highWater.get(BigInt(index + 1)) !== expected) {
        throw new Error(`Participant '${participant.key}' relocation replay acknowledgement is incomplete.`);
      }
    }
  }

  successorProgress(envelope: ServiceRelocationEnvelope): ServiceRelocationSuccessorProgress {
    const progress = new Map<string, import('../foundation/service-relocation-runtime').ServiceRelocationParticipantProgress>();
    const participants = this.staging?.envelope.participants
      ?? canonicalRelocationEnvelope(envelope).participants;
    for (const [index, participant] of participants.entries()) {
      progress.set(participant.key, {
        replayCursor: this.highWater.get(BigInt(index + 1)) ?? participant.replayCursor,
        terminalReplies: Buffer.alloc(0),
        pendingRelayCount: 0
      });
    }
    return progress;
  }

  private completion(
    staging: RemoteStaging,
    state: 'pending' | 'completed'
  ): Extract<ServiceMaintenanceRelocationControl, { kind: 'complete' }> {
    return { kind: 'complete', relocation: staging.relocation,
      targetAttemptGeneration: staging.targetAttemptGeneration, coordinator: this.coordinator,
      senderRole: 'source', source: coordinatorSource(this.coordinator), sourceCleanupState: state };
  }

  private stagingControl(
    staging: RemoteStaging,
    phase: 'prepared' | 'committed' | 'aborted'
  ): Extract<ServiceMaintenanceRelocationControl, { kind: 'data'; frozenRecord?: undefined }> {
    return {
      kind: 'data',
      relocation: staging.relocation,
      targetAttemptGeneration: staging.targetAttemptGeneration,
      coordinator: this.coordinator,
      senderRole: 'source',
      participantId: 1n,
      sequence: 1n,
      source: coordinatorSource(this.coordinator),
      object: staging.object,
      phase
    };
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
        participant.authorityOwnerGeneration,
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
        replayPackets: [],
        terminalDeliveries: new Map()
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
      participant.authorityOwnerGeneration,
      signal
    );
    return {
      authorityKey: participant.key,
      participant,
      activation,
      initialized: false,
      restoredTimers: [],
      replayResults: [],
      replayPackets: [],
      terminalDeliveries: new Map()
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
      const spotId = spotIdentity as RoutingId;
      state.setJoinedSpot(spotId, spot?.spot, membership.membershipEpoch);
      spot?.commitActorJoin(actor);
    }
  }

  async publish(hidden: LocalHidden): Promise<void> {
    if (hidden.actor !== undefined) {
      const store = this.requireLocationStore();
      const current = await requireAuthority(
        store,
        { value: hidden.authorityKey } as ZLinkAuthorityKey
      );
      const state = this.requireActorManager().getState(hidden.actor.context.actorId)!;
      state.setLocationGeneration(current.authorityOwnerGeneration);
      state.setOwnerLeaseGeneration(current.ownerLeaseGeneration);
      hidden.targetAuthorityOwnerGeneration = current.authorityOwnerGeneration;
      this.requireActorManager().publishRelocationActor(hidden.actor.context.actorId);
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
    }
    hidden.initialized = true;
  }

  async replayAcceptedJournal(hidden: LocalHidden, payload: Uint8Array): Promise<void> {
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
    if (state?.spotId === undefined) throw new Error('Relocated Actor membership is not staged.');
    const [result] = await replayActorHandoffBacklog(
      [packet],
      (parts, returnResponse, remoteBoundSessionTarget, fallbackActorRef) =>
        this.requireSpotManager().dispatchRoutedActorPacket(
          state.spotId!,
          hidden.actor!.context.actorId,
          parts,
          returnResponse,
          remoteBoundSessionTarget,
          fallbackActorRef
        )
    );
    hidden.replayResults.push(result!);
    hidden.replayPackets.push(packet);
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

  async openAdmission(hidden: LocalHidden): Promise<void> {
    if (hidden.actor !== undefined) {
      await this.options.actorTransfer.openRoutedActorSession(hidden.actor);
    }
  }

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
      const result = await store.read(relocationBlobReference(reference), signal);
      if (result.kind !== 'found') return { kind: 'missing' };
      const manifest = decodeRelocationStripeManifest(result.bytes);
      if (manifest === undefined) return { kind: 'found', payload: result.bytes };
      const stripes = await mapConcurrentOrdered(
        manifest.stripes,
        RELOCATION_STRIPE_CONCURRENCY,
        async stripe => {
          const read = await store.read(
            relocationBlobReference(stripe.reference),
            signal
          );
          if (read.kind !== 'found'
            || read.bytes.byteLength !== stripe.byteLength
            || crc32c(read.bytes) !== stripe.checksumCrc32c) {
            throw new Error(`Relocation stripe '${stripe.reference}' is missing or corrupt.`);
          }
          return Buffer.from(read.bytes);
        },
        signal
      );
      const payload = Buffer.concat(stripes);
      if (payload.byteLength !== manifest.byteLength
        || crc32c(payload) !== manifest.checksumCrc32c) {
        throw new Error('Relocation striped payload failed root integrity validation.');
      }
      return { kind: 'found', payload };
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

function primaryKey(envelope: ServiceRelocationEnvelope): string {
  return envelope.participants.find(value => value.objectKind !== 'actor')?.key
    ?? envelope.participants[0]!.key;
}

function isUserSpotAggregate(envelope: ServiceRelocationEnvelope): boolean {
  return envelope.participants.some(value => value.objectKind === 'user_spot');
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
  if (descriptor.activationConcurrency.active >= descriptor.activationConcurrency.limit) {
    return false;
  }
  const actorCount = requirements
    .filter(value => value.objectKind === 'actor')
    .reduce((sum, value) => sum + value.count, 0);
  const spotCount = requirements
    .filter(value => value.objectKind !== 'actor')
    .reduce((sum, value) => sum + value.count, 0);
  if (descriptor.populationCapacity.actors.active
      + descriptor.populationCapacity.actors.reserved
      + actorCount > descriptor.populationCapacity.actors.limit
    || descriptor.populationCapacity.spots.active
      + descriptor.populationCapacity.spots.reserved
      + spotCount > descriptor.populationCapacity.spots.limit) {
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
        || capacity.active + capacity.reserved + requirement.count > capacity.limit) {
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

function relocationPhaseRank(phase: LocalStage['phase']): number {
  return ['prepared', 'replayed', 'sealed', 'published', 'routed', 'normalized', 'open'].indexOf(phase);
}

function requireRelocationPhase(stage: LocalStage, minimum: LocalStage['phase']): void {
  if (relocationPhaseRank(stage.phase) < relocationPhaseRank(minimum)) {
    throw new Error(`Relocation staging requires '${minimum}' before continuing.`);
  }
}

interface RelocationTerminalCompletion {
  readonly index: number;
  readonly operationId: string;
  readonly source: NonNullable<ZLinkActorHandoffPacket['source']>;
  readonly result: ZLinkActorHandoffResult;
  readonly delivery: 'pending' | 'terminalReceived' | 'alreadyTerminal' | 'sourceLeaseExpired';
}

type RelocationTerminalDelivery = Exclude<RelocationTerminalCompletion['delivery'], 'pending'>;

interface DurableRelocationTerminalCompletion extends RelocationTerminalCompletion {
  readonly participantKey: string;
  readonly participantId: bigint;
}

function localSuccessorProgress(stage: LocalStage): ServiceRelocationSuccessorProgress {
  return successorProgressFromStaging(stage.staging);
}

function successorProgressFromStaging(
  staging: ServiceObjectRelocationStaging<LocalHidden>
): ServiceRelocationSuccessorProgress {
  const progress = new Map<string, import('../foundation/service-relocation-runtime').ServiceRelocationParticipantProgress>();
  for (const participant of staging.envelope.participants) {
    const hidden = staging.hidden.get(participant.key);
    const completions: RelocationTerminalCompletion[] = [];
    if (hidden?.actor !== undefined) {
      for (let index = 0; index < hidden.replayPackets.length; index++) {
        const packet = hidden.replayPackets[index]!;
        if (!packet.returnResponse || packet.source === undefined) continue;
        completions.push({ index: packet.index,
          operationId: packet.messageFollowContext.operationId,
          source: packet.source, result: hidden.replayResults[index]!,
          delivery: hidden.terminalDeliveries.get(packet.index) ?? 'pending' });
      }
    }
    progress.set(participant.key, {
      replayCursor: participant.queuedMessages.at(-1)?.sequence ?? participant.replayCursor,
      terminalReplies: encodeTerminalCompletions(completions),
      pendingRelayCount: completions.filter(value => value.delivery === 'pending').length
    });
  }
  return progress;
}

function encodeTerminalCompletions(values: readonly RelocationTerminalCompletion[]): Buffer {
  if (values.length === 0) return Buffer.alloc(0);
  return Buffer.from(JSON.stringify(values, (_key, value) =>
    typeof value === 'bigint' ? value.toString() : value), 'utf8');
}

function decodeTerminalCompletions(payload: Uint8Array): readonly RelocationTerminalCompletion[] {
  if (payload.byteLength === 0) return [];
  const parsed: unknown = JSON.parse(Buffer.from(payload).toString('utf8'));
  if (!Array.isArray(parsed)) throw new TypeError('Relocation terminal completion list is invalid.');
  return parsed.map((item, position) => {
    if (!isRecord(item) || !Number.isSafeInteger(item.index) || (item.index as number) < 0
      || typeof item.operationId !== 'string' || !/^\d+:\d+$/.test(item.operationId)
      || !isRecord(item.source) || typeof item.source.ownerId !== 'string'
      || typeof item.source.ownerLeaseGeneration !== 'string'
      || typeof item.source.nodeRid !== 'string' || typeof item.source.nodeGeneration !== 'string'
      || typeof item.source.replyRouteId !== 'string'
      || !isRecord(item.result) || item.result.index !== item.index || typeof item.result.ok !== 'boolean'
      || !['pending', 'terminalReceived', 'alreadyTerminal', 'sourceLeaseExpired'].includes(
        item.delivery as string
      )) {
      throw new TypeError(`Relocation terminal completion ${position} is invalid.`);
    }
    const source = {
      ownerId: item.source.ownerId,
      ownerLeaseGeneration: positiveDecimal(item.source.ownerLeaseGeneration, 'source owner lease'),
      nodeRid: item.source.nodeRid,
      nodeGeneration: positiveDecimal(item.source.nodeGeneration, 'source node generation'),
      replyRouteId: positiveDecimal(item.source.replyRouteId, 'reply route')
    };
    operationWireId(item.operationId);
    return {
      index: item.index as number,
      operationId: item.operationId,
      source,
      result: item.result as unknown as ZLinkActorHandoffResult,
      delivery: item.delivery as RelocationTerminalCompletion['delivery']
    };
  });
}

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

function sameRequestSource(
  left: ServiceWireRequestSourceFence,
  right: ServiceWireRequestSourceFence
): boolean {
  return left.ownerId === right.ownerId
    && left.leaseGeneration === right.leaseGeneration
    && left.nodeRid === right.nodeRid
    && left.nodeGeneration === right.nodeGeneration;
}

function sameHandoffSource(
  left: NonNullable<ZLinkActorHandoffPacket['source']>,
  right: NonNullable<ZLinkActorHandoffPacket['source']>
): boolean {
  return left.ownerId === right.ownerId
    && left.ownerLeaseGeneration === right.ownerLeaseGeneration
    && left.nodeRid === right.nodeRid
    && left.nodeGeneration === right.nodeGeneration
    && left.replyRouteId === right.replyRouteId;
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

function relocationParticipants(
  envelope: ServiceRelocationEnvelope,
  coordinator: ServiceWireRelocationCoordinatorFence,
  candidate: ServiceWireRelocationCandidate
): readonly ServiceWireRelocationParticipant[] {
  return envelope.participants.map((participant, index) => ({
    participantId: BigInt(index + 1),
    allowanceMessages: BigInt(participant.queuedMessages.length),
    allowanceBytes: BigInt(
      participant.applicationState.byteLength
      + participant.acceptedJournal.byteLength
      + participant.terminalReplies.byteLength
      + participant.timers.length * 128
    ) + participant.queuedMessages.reduce(
      (sum, message) => sum + BigInt(canonicalQueuedFrozenRecord(
        participant,
        message,
        coordinator,
        candidate
      ).canonicalBytes.byteLength), 0n)
  }));
}

function relocationEstimatedParticipants(
  envelope: ServiceRelocationEnvelope,
  expectedBytes: number
): readonly ServiceWireRelocationParticipant[] {
  const count = Math.max(1, envelope.participants.length);
  const bytesEach = BigInt(Math.ceil(expectedBytes / count));
  return envelope.participants.map((_participant, index) => ({
    participantId: BigInt(index + 1),
    allowanceMessages: index === 0 ? RELOCATION_MESSAGE_ESTIMATE_PER_UNIT : 0n,
    allowanceBytes: bytesEach
  }));
}

function relocationManifest(
  aggregateId: string,
  aggregateGeneration: bigint,
  units: readonly ServiceRelocationCaptureUnit[],
  memberships: readonly ServiceRelocationMembership[]
): ServiceRelocationEnvelope {
  return {
    aggregateId,
    aggregateGeneration,
    sourceCleanup: 'pending',
    participants: units.map(unit => ({
      key: unit.authorityKey,
      objectKind: unit.objectKind,
      stableType: unit.stableType,
      objectGeneration: unit.objectGeneration,
      authorityOwnerGeneration: unit.authorityOwnerGeneration,
      applicationState: Buffer.alloc(0),
      acceptedJournal: Buffer.alloc(0),
      replayCursor: 0n,
      terminalReplies: Buffer.alloc(0),
      pendingRelayCount: 0,
      queuedMessages: [],
      timers: []
    })),
    memberships
  };
}

/**
 * Uses the exact participant order persisted in the immutable root. Service
 * wire participant IDs are positional, so a source must not derive them from
 * the pre-encoding capture order when durable encoding has canonicalized it.
 * Payload buffers remain shared; relocation may be large enough that another
 * encode/decode copy would violate the in-flight byte bound.
 */
function canonicalRelocationEnvelope(
  envelope: ServiceRelocationEnvelope
): ServiceRelocationEnvelope {
  return {
    ...envelope,
    participants: [...envelope.participants]
      .sort((left, right) => left.key.localeCompare(right.key))
      .map(participant => ({
        ...participant,
        queuedMessages: [...participant.queuedMessages].sort((left, right) =>
          left.sequence < right.sequence ? -1 : left.sequence > right.sequence ? 1 : 0),
        timers: [...participant.timers].sort((left, right) =>
          left.timerId.localeCompare(right.timerId))
      })),
    memberships: [...envelope.memberships].sort((left, right) =>
      left.actorKey.localeCompare(right.actorKey))
  };
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
  candidate: ServiceWireRelocationCandidate
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
  return encodeServiceWireFrozenActorApplicationRecord({
    source,
    target: {
      actorId: identity,
      objectGeneration: participant.objectGeneration,
      nodeRid: candidate.nodeRid,
      nodeGeneration: candidate.nodeGeneration,
      authorityOwnerGeneration: participant.authorityOwnerGeneration + 1n,
      ownerLeaseGeneration: candidate.ownerLeaseGeneration
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

function relocationCapacityOffer(
  request: ServiceMaintenanceRelocationPrepare,
  envelope: ServiceRelocationEnvelope,
  offeredMessages: bigint,
  offeredBytes: bigint
): Extract<ServiceMaintenanceRelocationControl, { kind: 'ready' }> {
  return { kind: 'ready', relocation: request.relocation,
    targetAttemptGeneration: request.targetAttemptGeneration, round: request.round,
    coordinator: request.coordinator, candidate: request.candidate, object: request.object,
    role: 'target', offeredMessages, offeredBytes,
    participants: [], sourceNodeGeneration: request.sourceNodeGeneration,
    targetNodeGeneration: request.candidate.nodeGeneration,
    reservationGeneration: request.targetAttemptGeneration, root: request.root,
    applicationVersion: request.applicationVersion,
    participantProgress: envelope.participants.map((participant, index) => ({
      participantId: BigInt(index + 1),
      acceptedBoundary: participant.queuedMessages.at(-1)?.sequence ?? participant.replayCursor,
      replayCursor: participant.replayCursor
    })) };
}

function validateControlEnvelope(
  request: ServiceMaintenanceRelocationPrepare,
  envelope: ServiceRelocationEnvelope
): void {
  if (!sameWireId(request.relocation, relocationWireId(envelope.aggregateId))
    || request.targetAttemptGeneration !== envelope.aggregateGeneration) {
    throw new Error('Relocation prepare durable envelope identity changed.');
  }
  const expectedParticipants = relocationParticipants(
    envelope,
    request.coordinator,
    request.candidate
  );
  const initialManifest = request.round === 'initial';
  const validInitialInventory = initialManifest
    && request.participants.length === expectedParticipants.length
    && request.participants.every((participant, index) =>
      participant.participantId === BigInt(index + 1))
    && request.requiredMessages === request.participants.reduce(
      (sum, participant) => sum + participant.allowanceMessages, 0n)
    && request.requiredBytes <= request.participants.reduce(
      (sum, participant) => sum + participant.allowanceBytes, 0n);
  if (!validInitialInventory
    && stringifyWire(request.participants) !== stringifyWire(expectedParticipants)) {
    throw new Error(
      `Relocation prepare participant inventory changed: expected ${stringifyWire(expectedParticipants)}, received ${stringifyWire(request.participants)}.`
    );
  }
}

function validatePrepareOfferRetry(
  offer: TargetRelocationOffer,
  request: ServiceMaintenanceRelocationPrepare
): void {
  if (offer.prepareFingerprint !== stringifyWire(request)) {
    throw new Error('Relocation prepare retry changed its durable root or offer fence.');
  }
}

function validateReadyAcceptance(
  offer: TargetRelocationOffer,
  request: Extract<ServiceMaintenanceRelocationControl, { kind: 'ready' }>
): void {
  const participants = offer.prepare.participants;
  const targetOffer = relocationCapacityOffer(
    offer.prepare,
    offer.envelope,
    offer.offeredMessages,
    offer.offeredBytes
  );
  const expected = {
    ...targetOffer,
    role: 'source' as const,
    offeredMessages: 0n,
    offeredBytes: 0n,
    participants
  };
  if (stringifyWire(request) !== stringifyWire(expected)
    || participants.reduce((sum, value) => sum + value.allowanceMessages, 0n)
      > offer.offeredMessages
    || participants.reduce((sum, value) => sum + value.allowanceBytes, 0n)
      > offer.offeredBytes) {
    throw new Error('Relocation source acceptance changed the exact target offer.');
  }
}

function validateManifestReplacement(
  manifest: ServiceRelocationEnvelope,
  replacement: ServiceRelocationEnvelope
): void {
  const identity = (envelope: ServiceRelocationEnvelope) => ({
    aggregateId: envelope.aggregateId,
    aggregateGeneration: envelope.aggregateGeneration,
    participants: envelope.participants.map(participant => ({
      key: participant.key,
      objectKind: participant.objectKind,
      stableType: participant.stableType,
      objectGeneration: participant.objectGeneration,
      authorityOwnerGeneration: participant.authorityOwnerGeneration
    })),
    memberships: envelope.memberships
  });
  if (stringifyWire(identity(manifest)) !== stringifyWire(identity(replacement))) {
    throw new Error('Prepared relocation replacement changed its participant identity.');
  }
}

function validateStagingControl(
  offer: TargetRelocationOffer,
  request: ServiceMaintenanceRelocationControlData
): void {
  if (!sameCoordinator(offer.prepare.coordinator, request.coordinator)
    || stringifyWire(offer.prepare.object) !== stringifyWire(request.object)
    || request.source.nodeRid !== offer.authenticatedSourceNodeRid
    || request.source.nodeGeneration !== offer.prepare.sourceNodeGeneration) {
    throw new Error('Relocation staging control changed its prepare fence.');
  }
}

function relocationControlAck(
  request: ServiceMaintenanceRelocationControlData
): Extract<ServiceMaintenanceRelocationControl, { kind: 'ack' }> {
  return {
    kind: 'ack',
    relocation: request.relocation,
    targetAttemptGeneration: request.targetAttemptGeneration,
    coordinator: request.coordinator,
    senderRole: 'target',
    participantId: request.participantId,
    highWater: request.sequence
  };
}

function stringifyWire(value: unknown): string {
  return JSON.stringify(value, (_key, item) => typeof item === 'bigint' ? item.toString() : item);
}

function validateControlResponse(
  request: ZLinkServiceRelocationControlRequest,
  response: ZLinkServiceRelocationControlResponse
): void {
  const expected = request.kind === 'prepare' ? 'ready'
    : request.kind === 'ready' ? 'reserved'
      : request.kind === 'data' ? 'ack' : request.kind;
  if (response.kind !== expected
    || !sameWireId(response.relocation, request.relocation)
    || response.targetAttemptGeneration !== request.targetAttemptGeneration
    || !sameCoordinator(response.coordinator, request.coordinator)) {
    throw new Error('Relocation control response does not match the request fence.');
  }
}

function controlAckKey(request: ZLinkServiceRelocationControlRequest): string {
  const base = `relocation:${request.relocation.high}:${request.relocation.low}`;
  const attempt = `${base}:${request.targetAttemptGeneration}`;
  if (request.kind === 'prepare') return `${attempt}:ready`;
  if (request.kind === 'ready') return `${attempt}:reserved`;
  if (request.kind === 'data' || request.kind === 'ack') {
    return `${attempt}:ack:${request.participantId}`;
  }
  if (request.kind === 'seal') return `${attempt}:seal`;
  if (request.kind === 'complete') return `${attempt}:complete:${request.sourceCleanupState}`;
  return `${attempt}:${request.kind}`;
}

function controlResponseKey(packet: ZLinkServiceRelocationControlRequest): string | undefined {
  if (packet.kind === 'ready' && packet.role === 'target') {
    return `relocation:${packet.relocation.high}:${packet.relocation.low}:${packet.targetAttemptGeneration}:ready`;
  }
  if (packet.kind === 'reserved' || packet.kind === 'ack') return controlAckKey(packet);
  if (packet.kind === 'seal' && packet.senderRole === 'target') return controlAckKey(packet);
  if (packet.kind === 'complete' && packet.senderRole === 'target') return controlAckKey(packet);
  return undefined;
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

function positiveDecimal(value: string, name: string): string {
  if (!/^\d+$/.test(value) || BigInt(value) <= 0n) {
    throw new TypeError(`Relocation terminal ${name} is invalid.`);
  }
  return value;
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
    acceptedHighWater: target.acceptedHighWater?.toString(),
    relocationSealId: target.relocationSealId,
    acceptedJournalReference: target.acceptedJournalReference,
    acceptedJournalChecksumCrc32c: target.acceptedJournalChecksumCrc32c
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
  return {
    routerChannelId: value.routerChannelId,
    targetNodeRid: value.targetNodeRid as RoutingId,
    spotId: value.spotId as RoutingId,
    ...(typeof value.sessionNodeRid === 'string' ? { sessionNodeRid: value.sessionNodeRid as RoutingId } : {}),
    ...(typeof value.sessionRid === 'string' ? { sessionRid: value.sessionRid as RoutingId } : {}),
    ...(optionalBigInt('bindingGeneration') === undefined ? {} : { bindingGeneration: optionalBigInt('bindingGeneration') }),
    ...(optionalBigInt('previousAuthorityOwnerGeneration') === undefined ? {} : { previousAuthorityOwnerGeneration: optionalBigInt('previousAuthorityOwnerGeneration') }),
    ...(optionalBigInt('previousOwnerLeaseGeneration') === undefined ? {} : { previousOwnerLeaseGeneration: optionalBigInt('previousOwnerLeaseGeneration') }),
    ...(optionalBigInt('acceptedHighWater') === undefined ? {} : { acceptedHighWater: optionalBigInt('acceptedHighWater') }),
    ...(typeof value.relocationSealId === 'string' ? { relocationSealId: value.relocationSealId } : {}),
    ...(typeof value.acceptedJournalReference === 'string' ? { acceptedJournalReference: value.acceptedJournalReference } : {}),
    ...(typeof value.acceptedJournalChecksumCrc32c === 'number'
      ? { acceptedJournalChecksumCrc32c: value.acceptedJournalChecksumCrc32c }
      : {})
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
