import { ZLinkFrameworkInternalErrorKind, createInternalFrameworkException  } from '../framework-errors-internal';
import type {
  ActorRef,
  RoutingId,
  ZLinkActor,
  ZLinkActorContext,
  ZLinkSpot
} from '../../contracts';
import {
  ZLinkSpotKind
} from '../../contracts';
import type { Message } from '../../contracts/Common/Message';
import type {
  ZLinkBackendActorRef,
  ZLinkBackendMeshNode
} from '../backend/contracts';
import { routingIdsEqual } from '../routing-id';
import { lookupNativeActorRef } from './actor-native-lookup';

export interface ZLinkRemoteBoundSessionTarget {
  readonly routerChannelId: string;
  readonly targetNodeRid: RoutingId;
  readonly spotId: RoutingId;
  readonly sessionNodeRid?: RoutingId;
  readonly sessionRid?: RoutingId;
  readonly bindingGeneration?: bigint;
  readonly previousAuthorityOwnerGeneration?: bigint;
  readonly previousOwnerLeaseGeneration?: bigint;
  readonly acceptedHighWater?: bigint;
  readonly relocationSealId?: string;
  readonly acceptedJournalReference?: string;
  readonly acceptedJournalChecksumCrc32c?: number;
}

/**
 * Applies a lightweight Session binding refresh without discarding the
 * relocation fence that was staged for the same route. Core emits the bind
 * refresh with routing coordinates only; the relocation owner retains the
 * seal and accepted-journal fields until command 46 releases them.
 */
export function mergeRemoteBoundSessionTarget(
  target: ZLinkRemoteBoundSessionTarget,
  fallback: ZLinkRemoteBoundSessionTarget | undefined
): ZLinkRemoteBoundSessionTarget {
  if (
    fallback === undefined
    || fallback.routerChannelId !== target.routerChannelId
    || !routingIdsEqual(fallback.targetNodeRid, target.targetNodeRid)
    || !routingIdsEqual(fallback.spotId, target.spotId)
  ) {
    return target;
  }
  const merged = {
    ...fallback,
    ...target,
    sessionNodeRid: target.sessionNodeRid ?? fallback.sessionNodeRid,
    sessionRid: target.sessionRid ?? fallback.sessionRid,
    bindingGeneration: target.bindingGeneration ?? fallback.bindingGeneration,
    previousAuthorityOwnerGeneration:
      target.previousAuthorityOwnerGeneration ?? fallback.previousAuthorityOwnerGeneration,
    previousOwnerLeaseGeneration:
      target.previousOwnerLeaseGeneration ?? fallback.previousOwnerLeaseGeneration,
    acceptedHighWater: target.acceptedHighWater ?? fallback.acceptedHighWater,
    relocationSealId: target.relocationSealId ?? fallback.relocationSealId,
    acceptedJournalReference: target.acceptedJournalReference ?? fallback.acceptedJournalReference,
    acceptedJournalChecksumCrc32c:
      target.acceptedJournalChecksumCrc32c ?? fallback.acceptedJournalChecksumCrc32c
  };
  return merged;
}

/**
 * Selects the route that still carries the relocation fence when a Core
 * binding refresh and a formal transfer target temporarily coexist.
 */
export function preferredRemoteBoundSessionTarget(
  remote: ZLinkRemoteBoundSessionTarget | undefined,
  transfer: ZLinkRemoteBoundSessionTarget | undefined
): ZLinkRemoteBoundSessionTarget | undefined {
  if (remote === undefined) return transfer;
  if (hasRelocationFence(remote) || transfer === undefined) return remote;
  return transfer;
}

function hasRelocationFence(target: ZLinkRemoteBoundSessionTarget): boolean {
  return target.acceptedHighWater !== undefined
    || target.relocationSealId !== undefined
    || target.acceptedJournalReference !== undefined
    || target.acceptedJournalChecksumCrc32c !== undefined;
}

export interface ZLinkRemoteActorPacketTarget {
  readonly routerChannelId: string;
  readonly targetNodeRid: RoutingId;
  readonly spotId: RoutingId;
  readonly spotKind?: ZLinkSpotKind;
  readonly targetSpotGeneration?: bigint;
}

export interface ZLinkActorCreationOperation {
  readonly task: Promise<ZLinkActorCreationAttemptResult>;
  readonly created: boolean;
}

export type ZLinkActorCreationAttemptResult =
  | {
      readonly status: 'created';
      readonly actor: ZLinkActor;
      readonly reply?: unknown;
    }
  | {
      readonly status: 'rejected';
      readonly reply?: unknown;
    };

export class ZLinkActorRuntimeState {
  private creationTask: Promise<ZLinkActorCreationAttemptResult> | undefined;
  private configured = false;
  private context: ZLinkActorContext | undefined;
  private actorTypeValue: string | undefined;
  private meshNameValue: string | undefined;
  private actorValue: ZLinkActor | undefined;
  private spotValue: ZLinkSpot | undefined;
  private spotIdValue: RoutingId | undefined;
  private spotGenerationValue: bigint | undefined;
  private spotMembershipEpochValue = 0n;
  private nativeActorRefValue: ZLinkBackendActorRef | undefined;
  private boundSessionBindingGenerationValue = 0n;
  private entryNodeRidValue: RoutingId | undefined;
  private remoteBoundSessionTargetValue: ZLinkRemoteBoundSessionTarget | undefined;
  private boundSessionTransferTargetValue: ZLinkRemoteBoundSessionTarget | undefined;
  private remoteActorPacketTargetValue: ZLinkRemoteActorPacketTarget | undefined;
  private createRequestPayloadValue: Buffer | undefined;
  private ownsLocationValue = false;
  private locationGenerationValue: bigint | undefined;
  private ownerLeaseGenerationValue: bigint | undefined;
  private movingValue = false;
  private deferredJoinPendingValue = false;
  private destroyTask: Promise<void> | undefined;

  constructor(readonly actorId: string) {}

  get actorType(): string | undefined {
    return this.actorTypeValue;
  }

  // The manager resolves RouteMesh membership once when it registers the
  // actor, so identity reads never re-enter the application provider.
  get meshName(): string | undefined {
    return this.meshNameValue;
  }

  rememberMeshName(meshName: string): void {
    this.meshNameValue = meshName;
  }

  get actor(): ZLinkActor | undefined {
    return this.actorValue;
  }

  get spot(): ZLinkSpot | undefined {
    return this.spotValue;
  }

  get spotId(): RoutingId | undefined {
    return this.spotIdValue;
  }

  get spotMembershipEpoch(): bigint {
    return this.spotMembershipEpochValue;
  }

  get spotGeneration(): bigint | undefined {
    return this.spotGenerationValue;
  }

  get nativeActorRef(): ZLinkBackendActorRef | undefined {
    return this.nativeActorRefValue;
  }

  get boundSessionBindingGeneration(): bigint {
    return this.boundSessionBindingGenerationValue;
  }

  get entryNodeRid(): RoutingId | undefined {
    return this.entryNodeRidValue;
  }

  get remoteBoundSessionTarget(): ZLinkRemoteBoundSessionTarget | undefined {
    return this.remoteBoundSessionTargetValue;
  }

  get boundSessionTransferTarget(): ZLinkRemoteBoundSessionTarget | undefined {
    return this.boundSessionTransferTargetValue;
  }

  get remoteActorPacketTarget(): ZLinkRemoteActorPacketTarget | undefined {
    return this.remoteActorPacketTargetValue;
  }

  get createRequestPayload(): Buffer | undefined {
    return this.createRequestPayloadValue;
  }

  get isJoined(): boolean {
    return this.spotIdValue !== undefined;
  }

  get ownsLocation(): boolean {
    return this.ownsLocationValue;
  }

  get locationGeneration(): bigint | undefined {
    return this.locationGenerationValue;
  }

  get ownerLeaseGeneration(): bigint | undefined {
    return this.ownerLeaseGenerationValue;
  }

  get isMoving(): boolean {
    return this.movingValue;
  }

  tryBeginDeferredJoin(): boolean {
    if (this.movingValue || this.deferredJoinPendingValue) {
      return false;
    }
    this.deferredJoinPendingValue = true;
    return true;
  }

  endDeferredJoin(): void {
    this.deferredJoinPendingValue = false;
  }

  get hasActorOrCreation(): boolean {
    return this.actorValue !== undefined || this.creationTask !== undefined;
  }

  beginMove(): void {
    if (this.deferredJoinPendingValue) {
      throw createInternalFrameworkException(
        ZLinkFrameworkInternalErrorKind.ActorMoving,
        `Actor '${this.actorId}' has a pending membership transition.`,
        true
      );
    }
    if (this.movingValue) {
      throw createInternalFrameworkException(
        ZLinkFrameworkInternalErrorKind.ActorRouteNotFound,
        `Actor '${this.actorId}' is already moving.`
      );
    }
    this.movingValue = true;
  }

  endMove(): void {
    this.movingValue = false;
  }

  markLocationOwned(): void {
    this.ownsLocationValue = true;
  }

  markLocationReleased(): void {
    this.ownsLocationValue = false;
  }

  setLocationGeneration(generation: bigint): void {
    this.locationGenerationValue = generation;
  }

  setOwnerLeaseGeneration(generation: bigint): void {
    this.ownerLeaseGenerationValue = generation;
  }

  getOrStartDestroy(
    entryNodeRid: RoutingId,
    destroy: (actorRef: ZLinkBackendActorRef | undefined) => Promise<void>
  ): Promise<void> {
    if (this.destroyTask !== undefined) {
      return this.destroyTask;
    }
    const actorRef = this.nativeActorRefValue;
    if (actorRef !== undefined && !routingIdsEqual(toFrameworkRoutingId(actorRef.nodeRid), entryNodeRid)) {
      throw createInternalFrameworkException(
        ZLinkFrameworkInternalErrorKind.ActorRouteNotFound,
        `Actor '${this.actorId}' is not owned by this Entry Spot.`
      );
    }

    const task = Promise.resolve().then(() => destroy(actorRef));
    this.destroyTask = task;
    return task;
  }

  markNativeActorDestroyed(actorRef: ZLinkBackendActorRef): void {
    if (this.nativeActorRefValue === actorRef) {
      this.nativeActorRefValue = undefined;
    }
  }

  clearFailedDestroy(task: Promise<void>): void {
    if (this.destroyTask === task) {
      this.destroyTask = undefined;
    }
    this.movingValue = false;
  }

  ensureContext(createContext: () => ZLinkActorContext): ZLinkActorContext {
    this.context ??= createContext();
    return this.context;
  }

  getOrStartCreation(
    actorType: string,
    failIfExists: boolean,
    createActor: () => Promise<ZLinkActorCreationAttemptResult>
  ): ZLinkActorCreationOperation {
    if (this.actorTypeValue !== undefined && this.actorTypeValue !== actorType) {
      throw createInternalFrameworkException(
        ZLinkFrameworkInternalErrorKind.ActorTypeMismatch,
        `Actor '${this.actorId}' already uses actor type '${this.actorTypeValue}', not '${actorType}'.`
      );
    }

    if (this.actorValue !== undefined) {
      if (failIfExists) {
        throw createInternalFrameworkException(
          ZLinkFrameworkInternalErrorKind.ActorAlreadyExists,
          `Actor '${this.actorId}' already exists.`
        );
      }
      return {
        task: Promise.resolve({ status: 'created', actor: this.actorValue }),
        created: false
      };
    }

    if (this.creationTask !== undefined) {
      if (failIfExists) {
        throw createInternalFrameworkException(
          ZLinkFrameworkInternalErrorKind.ActorAlreadyExists,
          `Actor '${this.actorId}' is already being created.`
        );
      }
      return { task: this.creationTask, created: false };
    }

    this.actorTypeValue = actorType;
    this.creationTask = createActor();
    return { task: this.creationTask, created: true };
  }

  clearFailedCreation(task: Promise<ZLinkActorCreationAttemptResult>): boolean {
    if (this.creationTask === task && this.actorValue === undefined) {
      this.creationTask = undefined;
      this.actorTypeValue = undefined;
      this.createRequestPayloadValue = undefined;
      this.configured = false;
      this.nativeActorRefValue = undefined;
      this.entryNodeRidValue = undefined;
      return true;
    }
    return false;
  }

  bindActor(actor: ZLinkActor, context: ZLinkActorContext): void {
    if (actor.context.actorId !== this.actorId) {
      throw createInternalFrameworkException(
        ZLinkFrameworkInternalErrorKind.ActorCreateFailed,
        `Actor state id '${this.actorId}' does not match actor id '${actor.context.actorId}'.`
      );
    }
    if (actor.context !== context) {
      throw createInternalFrameworkException(
        ZLinkFrameworkInternalErrorKind.ActorCreateFailed,
        `Actor '${this.actorId}' must expose the context provided by its factory.`
      );
    }

    this.actorValue = actor;
    this.creationTask = undefined;
    if (!this.configured) {
      actor.configure?.();
      this.configured = true;
    }
  }

  ensureNativeActorRef(node: ZLinkBackendMeshNode, request?: Message): ZLinkBackendActorRef {
    if (this.nativeActorRefValue === undefined) {
      const existing = lookupNativeActorRef(node, this.actorId);
      const native = existing ?? node.createActor(
        this.actorId,
        request === undefined ? undefined : Buffer.from(request.data())
      );
      this.nativeActorRefValue = {
        nodeRid: native.nodeRid as ZLinkBackendActorRef['nodeRid'],
        actorId: native.actorId,
        generation: native.generation
      };
    }
    this.entryNodeRidValue ??= toFrameworkRoutingId(this.nativeActorRefValue.nodeRid);
    return this.nativeActorRefValue;
  }

  setNativeActorRef(actorRef: ZLinkBackendActorRef): void {
    this.nativeActorRefValue = actorRef;
    this.entryNodeRidValue ??= toFrameworkRoutingId(actorRef.nodeRid);
  }

  setBoundSessionBindingGeneration(generation: bigint): void {
    if (generation > this.boundSessionBindingGenerationValue) {
      this.boundSessionBindingGenerationValue = generation;
      if (this.remoteBoundSessionTargetValue !== undefined) {
        this.remoteBoundSessionTargetValue = {
          ...this.remoteBoundSessionTargetValue,
          bindingGeneration: generation
        };
      }
      if (this.boundSessionTransferTargetValue !== undefined) {
        this.boundSessionTransferTargetValue = {
          ...this.boundSessionTransferTargetValue,
          bindingGeneration: generation
        };
      }
    }
  }

  setEntryNodeRid(entryNodeRid: RoutingId): void {
    this.entryNodeRidValue = entryNodeRid;
  }

  setRemoteBoundSessionTarget(target: ZLinkRemoteBoundSessionTarget | undefined): void {
    const current = preferredRemoteBoundSessionTarget(
      this.remoteBoundSessionTargetValue,
      this.boundSessionTransferTargetValue
    );
    const merged = target === undefined
      ? undefined
      : mergeRemoteBoundSessionTarget(target, current);
    const bindingGeneration = maxBindingGeneration(
      merged?.bindingGeneration,
      this.remoteBoundSessionTargetValue?.bindingGeneration,
      this.boundSessionBindingGenerationValue
    );
    this.remoteBoundSessionTargetValue = merged === undefined
      ? undefined
      : bindingGeneration === undefined
        ? merged
        : { ...merged, bindingGeneration };
  }

  setBoundSessionTransferTarget(target: ZLinkRemoteBoundSessionTarget | undefined): void {
    const bindingGeneration = maxBindingGeneration(
      target?.bindingGeneration,
      this.boundSessionTransferTargetValue?.bindingGeneration,
      this.boundSessionBindingGenerationValue
    );
    this.boundSessionTransferTargetValue = target === undefined
      ? undefined
      : bindingGeneration === undefined
        ? target
        : { ...target, bindingGeneration };
  }

  setRemoteActorPacketTarget(target: ZLinkRemoteActorPacketTarget | undefined): void {
    this.remoteActorPacketTargetValue = target;
  }

  setCreateRequestPayload(payload: Buffer | Uint8Array): void {
    this.createRequestPayloadValue = Buffer.from(payload);
  }

  setJoinedSpot(
    spotId: RoutingId,
    spot?: ZLinkSpot,
    membershipEpoch = 0n,
    spotGeneration?: bigint
  ): void {
    this.spotIdValue = spotId;
    this.spotValue = spot;
    if (spotGeneration !== undefined && spotGeneration > 0n) {
      this.spotGenerationValue = spotGeneration;
    }
    this.spotMembershipEpochValue = membershipEpoch;
  }

  clearJoinedSpot(): void {
    this.spotIdValue = undefined;
    this.spotValue = undefined;
    this.spotGenerationValue = undefined;
    this.spotMembershipEpochValue = 0n;
  }

  clearAfterDestroy(): void {
    this.creationTask = undefined;
    this.configured = false;
    this.context = undefined;
    this.actorTypeValue = undefined;
    this.actorValue = undefined;
    this.spotValue = undefined;
    this.spotIdValue = undefined;
    this.spotGenerationValue = undefined;
    this.spotMembershipEpochValue = 0n;
    this.nativeActorRefValue = undefined;
    this.boundSessionBindingGenerationValue = 0n;
    this.entryNodeRidValue = undefined;
    this.remoteBoundSessionTargetValue = undefined;
    this.boundSessionTransferTargetValue = undefined;
    this.remoteActorPacketTargetValue = undefined;
    this.createRequestPayloadValue = undefined;
    this.ownsLocationValue = false;
    this.locationGenerationValue = undefined;
    this.ownerLeaseGenerationValue = undefined;
    this.movingValue = false;
    this.deferredJoinPendingValue = false;
    this.destroyTask = undefined;
  }

  prepareForRemoteReentry(): void {
    if (this.remoteActorPacketTargetValue === undefined) return;
    this.creationTask = undefined;
    this.configured = false;
    this.context = undefined;
    this.actorTypeValue = undefined;
    this.actorValue = undefined;
    this.spotValue = undefined;
    this.spotIdValue = undefined;
    this.nativeActorRefValue = undefined;
    this.boundSessionBindingGenerationValue = 0n;
    this.remoteBoundSessionTargetValue = undefined;
    this.boundSessionTransferTargetValue = undefined;
    this.remoteActorPacketTargetValue = undefined;
    this.createRequestPayloadValue = undefined;
    this.ownsLocationValue = false;
    this.locationGenerationValue = undefined;
    this.ownerLeaseGenerationValue = undefined;
    this.movingValue = false;
    this.deferredJoinPendingValue = false;
    this.destroyTask = undefined;
  }
}

function maxBindingGeneration(...values: (bigint | undefined)[]): bigint | undefined {
  let maximum: bigint | undefined;
  for (const value of values) {
    if (value === undefined || value <= 0n) continue;
    if (maximum === undefined || value > maximum) maximum = value;
  }
  return maximum;
}

export function toFrameworkRoutingId(routingId: unknown): RoutingId {
  // Runtime routing identities are opaque binary values. Preserve binding
  // RoutingId instances so a later native call cannot reinterpret their
  // hexadecimal display string as different literal bytes.
  return routingId as unknown as RoutingId;
}

export function toFrameworkActorRef(
  actor: ZLinkBackendActorRef,
  meshName: string
): ActorRef {
  const actorRef = {
    actorId: actor.actorId,
    objectGeneration: actor.generation,
    meshName,
    nodeRid: toFrameworkRoutingId(actor.nodeRid)
  };
  Object.defineProperty(actorRef, 'generation', {
    configurable: false,
    enumerable: false,
    value: actor.generation
  });
  return actorRef;
}
