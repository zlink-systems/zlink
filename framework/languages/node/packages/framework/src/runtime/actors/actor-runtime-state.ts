import {
  ZLinkFrameworkInternalErrorKind,
  createInternalFrameworkException
} from '../framework-errors-internal';
import type {
  ActorRef,
  RoutingId,
  ZLinkActor,
  ZLinkActorContext,
  ZLinkSpot
} from '../../contracts';
import { ZLinkSpotKind } from '../../contracts';
import type { Message } from '../../contracts/Common/Message';
import type { ZLinkBackendActorRef, ZLinkBackendMeshNode } from '../backend/contracts';
import { routingIdsEqual } from '../routing-id';
import { lookupNativeActorRef } from './actor-native-lookup';

export interface ZLinkRemoteBoundSessionTarget {
  readonly routerChannelId: string;
  readonly targetNodeRid: RoutingId;
  readonly spotId: RoutingId;
  readonly sessionNodeRid?: RoutingId;
  readonly sessionRid?: RoutingId;
  readonly sessionOwnerNodeGeneration?: bigint;
  readonly sessionOwnerId?: string;
  readonly sessionOwnerLeaseGeneration?: bigint;
  readonly bindingGeneration?: bigint;
  readonly previousAuthorityOwnerGeneration?: bigint;
  readonly previousOwnerLeaseGeneration?: bigint;
  readonly relocationSealId?: string;
  readonly serviceWireRelocation?: {
    readonly relocation: {
      readonly high: bigint;
      readonly low: bigint;
    };
    readonly coordinator: {
      readonly ownerId: string;
      readonly leaseGeneration: bigint;
      readonly nodeRid: string;
      readonly nodeGeneration: bigint;
      readonly expectedAuthorityStoreVersion: string;
    };
    readonly session: {
      readonly sessionOwnerNodeRid: string;
      readonly sessionOwnerNodeGeneration: bigint;
      readonly sessionOwnerId: string;
      readonly sessionOwnerLeaseGeneration: bigint;
      readonly sessionRid: string;
      readonly bindingGeneration: bigint;
    };
  };
}

/**
 * Session–Actor binding §5·§6: the Actor owner keeps one current binding. The
 * stored value is the binding identity and the route to its Session owner; a
 * relocation fence belongs to the relocation operation that created it.
 */
function boundSessionIdentity(
  target: ZLinkRemoteBoundSessionTarget
): ZLinkRemoteBoundSessionTarget {
  return {
    routerChannelId: target.routerChannelId,
    targetNodeRid: target.targetNodeRid,
    spotId: target.spotId,
    sessionNodeRid: target.sessionNodeRid,
    sessionRid: target.sessionRid,
    sessionOwnerNodeGeneration: target.sessionOwnerNodeGeneration,
    sessionOwnerId: target.sessionOwnerId,
    sessionOwnerLeaseGeneration: target.sessionOwnerLeaseGeneration,
    bindingGeneration: target.bindingGeneration
  };
}

type ZLinkRemoteSessionOwnerIdentity = Pick<
  ZLinkRemoteBoundSessionTarget,
  | 'sessionNodeRid'
  | 'sessionRid'
  | 'sessionOwnerNodeGeneration'
  | 'sessionOwnerId'
  | 'sessionOwnerLeaseGeneration'
>;

type ZLinkRemoteSessionOwnerLifecycle = Omit<ZLinkRemoteSessionOwnerIdentity, 'sessionRid'>;

function sameRemoteSessionOwnerLifecycle(
  left: ZLinkRemoteBoundSessionTarget,
  right: ZLinkRemoteSessionOwnerLifecycle
): boolean {
  return (
    left.sessionNodeRid !== undefined &&
    right.sessionNodeRid !== undefined &&
    left.sessionOwnerNodeGeneration !== undefined &&
    right.sessionOwnerNodeGeneration !== undefined &&
    left.sessionOwnerId !== undefined &&
    right.sessionOwnerId !== undefined &&
    left.sessionOwnerLeaseGeneration !== undefined &&
    right.sessionOwnerLeaseGeneration !== undefined &&
    routingIdsEqual(left.sessionNodeRid, right.sessionNodeRid) &&
    left.sessionOwnerNodeGeneration === right.sessionOwnerNodeGeneration &&
    left.sessionOwnerId === right.sessionOwnerId &&
    left.sessionOwnerLeaseGeneration === right.sessionOwnerLeaseGeneration
  );
}

function sameRemoteSessionOwner(
  left: ZLinkRemoteBoundSessionTarget,
  right: ZLinkRemoteSessionOwnerIdentity
): boolean {
  return (
    left.sessionRid !== undefined &&
    right.sessionRid !== undefined &&
    routingIdsEqual(left.sessionRid, right.sessionRid) &&
    sameRemoteSessionOwnerLifecycle(left, right)
  );
}

type ZLinkBoundSessionBindingIdentity = Pick<
  ZLinkRemoteBoundSessionTarget,
  keyof ZLinkRemoteSessionOwnerIdentity | 'bindingGeneration'
>;

function sameRemoteSessionBinding(
  left: ZLinkRemoteBoundSessionTarget,
  right: ZLinkBoundSessionBindingIdentity
): boolean {
  return (
    sameRemoteSessionOwner(left, right) &&
    left.bindingGeneration !== undefined &&
    right.bindingGeneration !== undefined &&
    left.bindingGeneration === right.bindingGeneration
  );
}

export interface ZLinkRemoteActorPacketTarget {
  readonly routerChannelId: string;
  readonly targetNodeRid: RoutingId;
  readonly spotId: RoutingId;
  readonly spotKind?: ZLinkSpotKind;
  readonly targetSpotGeneration?: bigint;
  readonly targetNodeGeneration?: bigint;
  readonly authorityOwnerGeneration?: bigint;
  readonly targetOwnerId?: string;
  readonly ownerLeaseGeneration?: bigint;
  readonly authorityStoreVersion?: string;
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
    }
  | {
      readonly status: 'failed';
      readonly error: unknown;
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
  private entryNodeRidValue: RoutingId | undefined;
  private boundSessionValue: ZLinkRemoteBoundSessionTarget | undefined;
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
    return this.boundSessionValue?.bindingGeneration ?? 0n;
  }

  get entryNodeRid(): RoutingId | undefined {
    return this.entryNodeRidValue;
  }

  /** The one current Session binding of this Actor (Session–Actor binding §6). */
  get boundSession(): ZLinkRemoteBoundSessionTarget | undefined {
    return this.boundSessionValue;
  }

  /**
   * The current binding when its Session owner is another node. A same-node
   * binding is served by the local Session registry, so remote routing sees
   * no target for it.
   */
  get remoteBoundSessionTarget(): ZLinkRemoteBoundSessionTarget | undefined {
    const binding = this.boundSessionValue;
    const actorNodeRid = this.nativeActorRefValue?.nodeRid;
    return binding?.sessionNodeRid !== undefined &&
      actorNodeRid !== undefined &&
      routingIdsEqual(binding.sessionNodeRid, toFrameworkRoutingId(actorNodeRid))
      ? undefined
      : binding;
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
    if (
      actorRef !== undefined &&
      !routingIdsEqual(toFrameworkRoutingId(actorRef.nodeRid), entryNodeRid)
    ) {
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
      const native =
        existing ??
        node.createActor(
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

  /** @internal Installs one authority-confirmed binding inside its Actor owner turn. */
  installBoundSessionBinding(target: ZLinkRemoteBoundSessionTarget): boolean {
    if (
      target.sessionNodeRid === undefined ||
      target.sessionRid === undefined ||
      target.sessionOwnerNodeGeneration === undefined ||
      target.sessionOwnerId === undefined ||
      target.sessionOwnerLeaseGeneration === undefined ||
      target.bindingGeneration === undefined
    ) {
      throw new TypeError('An installed bound Session target requires its binding identity.');
    }
    const current = this.boundSessionValue;
    if (current !== undefined && sameRemoteSessionOwnerLifecycle(current, target)) {
      // A duplicate or late older command for the exact physical Session
      // leaves the current binding; a newer generation of the same Session
      // replaces it. A different Session in the same owner lifecycle must
      // advance that lifecycle's generation; a late equal/lower one is stale.
      const advances =
        current.bindingGeneration === undefined ||
        target.bindingGeneration > current.bindingGeneration;
      if (!advances) return sameRemoteSessionOwner(current, target);
    }
    // Binding generations are scoped to a Session-owner lifecycle. A restarted
    // owner can legitimately reset its counter below the predecessor value.
    this.boundSessionValue = boundSessionIdentity(target);
    return true;
  }

  /** @internal Removes only the exact authority-confirmed Session incarnation. */
  retireBoundSessionBinding(target: ZLinkBoundSessionBindingIdentity): boolean {
    if (
      target.sessionNodeRid === undefined ||
      target.sessionRid === undefined ||
      target.sessionOwnerNodeGeneration === undefined ||
      target.sessionOwnerId === undefined ||
      target.sessionOwnerLeaseGeneration === undefined ||
      target.bindingGeneration === undefined
    ) {
      throw new TypeError('A retired bound Session target requires its binding identity.');
    }
    const current = this.boundSessionValue;
    if (current === undefined || !sameRemoteSessionBinding(current, target)) return false;
    this.boundSessionValue = undefined;
    return true;
  }

  setEntryNodeRid(entryNodeRid: RoutingId): void {
    this.entryNodeRidValue = entryNodeRid;
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
    this.entryNodeRidValue = undefined;
    this.boundSessionValue = undefined;
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
    // The arriving relocation installs the binding it carries.
    this.boundSessionValue = undefined;
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

export function toFrameworkRoutingId(routingId: unknown): RoutingId {
  // Runtime routing identities are opaque binary values. Preserve binding
  // RoutingId instances so a later native call cannot reinterpret their
  // hexadecimal display string as different literal bytes.
  return routingId as unknown as RoutingId;
}

export function toFrameworkActorRef(actor: ZLinkBackendActorRef, meshName: string): ActorRef {
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
