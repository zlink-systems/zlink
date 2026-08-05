import {
  ZLinkFrameworkInternalErrorKind,
  createInternalFrameworkException,
  internalFrameworkErrorCode,
  internalFrameworkErrorKind
} from '../framework-errors-internal';
import { RequestResult, SubmitResult } from '../backend/runtime-values';
import type {
  RawServiceIngressRecord,
  RawServiceMeshRuntime,
  RawServicePumpResult
} from './raw-service-mesh-runtime';
import {
  ActorLifecycleKind,
  OperationKind,
  ReceiveKind,
  type ActorControlPayload,
  type ActorJoinCompletionPayload,
  type ActorLocation,
  type ReceiveKindData
} from './service-runtime-contracts';
import type { ServiceMailboxRecord } from './service-mailbox';
import {
  ServiceStaleGenerationError,
  ServiceStatefulRegistry,
  ServiceTerminalOperationRegistry,
  type ServiceActorRef,
  type ServiceActorState,
  type ServiceSessionBinding,
  type ServiceSpotRef,
  type ServiceSpotState
} from './service-stateful-registry';
import {
  decodeStatefulHeader,
  decodeStatefulReply,
  encodeActorCreateHeader,
  encodeActorDestroyHeader,
  encodeActorHeader,
  encodeActorJoinHeader,
  encodeActorLookupHeader,
  encodeBoundSessionBindHeader,
  encodeBoundSessionSendHeader,
  encodeInstanceSpotActivationHeader,
  encodeInstanceSpotHeader,
  encodeLogicalMulticastHeader,
  encodeMessageFollowHeader,
  encodeSpotHeader,
  encodeStatefulReply,
  encodeUserSpotCloseHeader,
  encodeUserSpotCreateHeader,
  M6bServiceWireCommand,
  M6bServiceWireFlag,
  sessionBindingFromWire,
  type ServiceActorRouteFence,
  type ServiceActorCreateRecord,
  type ServiceInstanceActivationTarget,
  type ServiceInstanceRouteFence,
  type ServiceMessageFollowRoute,
  type ServiceDirectSpotRouteFence,
  type ServiceSpotRouteFence,
  type ServiceStatefulReplyTail,
  type ServiceStatefulWireRecord,
  type ServiceUserSpotCloseRecord,
  type ServiceUserSpotCreateRecord
} from './service-stateful-wire-codec';

import {
  decodeApplicationPayload,
  encodeApplicationPayload,
  type ServiceApplicationPayload,
  ServiceWireProtocolError
} from './service-wire-m6a-codec';
import type {
  ServiceInstanceActivationRecoveryEnvelope
} from './service-instance-activation-recovery-codec';
import { validateServiceMetadataFrame } from './service-metadata-codec';
import {
  ZLinkFrameworkException
} from '../../contracts';
import { appendFileSync } from 'node:fs';

const ACTOR_ROUTE_STALE = 21;
const SPOT_MOVING = 34;
const USER_SPOT_OPERATION_CAPACITY = 65_536;
const USER_SPOT_OPERATION_REPLAY_RETENTION_MS = 5 * 60_000;
const ACTOR_ROUTE_NOT_FOUND = 1;
const MESSAGE_FOLLOW_MESSAGE_LIMIT = 1024;
const MESSAGE_FOLLOW_BYTE_LIMIT = 16 * 1024 * 1024;

export interface ServiceSpotMessageFollowSeal {
  readonly key: string;
  readonly serial: bigint;
}

interface ServiceSpotMessageFollowRecord {
  readonly ingress: RawServiceIngressRecord;
  readonly wire: Extract<
    ServiceStatefulWireRecord,
    { readonly kind: 'spotSend' | 'spotRequest' }
  >;
  readonly bytes: number;
}

interface ServiceSpotMessageFollowState {
  readonly seal: ServiceSpotMessageFollowSeal;
  readonly source: ServiceDirectSpotRouteFence;
  readonly queued: Array<ServiceSpotMessageFollowRecord | undefined>;
  queuedHead: number;
  queuedCount: number;
  queuedBytes: number;
  target?: ServiceDirectSpotRouteFence;
  expiresAtMs?: number;
  draining: boolean;
  retryTimer?: ReturnType<typeof setTimeout>;
}

export interface ServiceStatefulResult {
  readonly terminalResult: number;
  readonly failureCode: number;
  readonly payload?: ServiceApplicationPayload;
  readonly kindData?: ReceiveKindData;
  readonly applicationMetadata?: Buffer;
}

export interface ServiceStatefulPendingOperation {
  readonly id: bigint;
  readonly promise: Promise<ServiceStatefulResult>;
}

export interface ServiceStatefulMailboxData {
  readonly correlation?: bigint;
  readonly receiveKind: number;
  readonly operationKind: number;
  readonly sourceSpotId?: string;
  readonly sourceActor?: ServiceActorRef;
  readonly sourceBindingGeneration?: bigint;
  readonly channelName?: string;
  readonly topic?: string;
  readonly targetSpot?: ServiceSpotRef;
  readonly targetActor?: ServiceActorRef;
  readonly kindData?: ReceiveKindData;
  readonly applicationMetadata?: Buffer;
  readonly messageFollowOrigin?: import('./service-runtime-contracts').ZLinkMessageFollowOrigin;
  /** Prevents a local lifecycle callback from publishing after its operation ended. */
  readonly isPending?: () => boolean;
  /** Carries the local operation deadline into the lifecycle dispatch lane. */
  readonly deadlineUnixMs?: bigint;
  readonly onTerminalCompletion?: () => void | Promise<void>;
  readonly reply?: (
    terminalResult: number,
    failureCode: number,
    payload?: ServiceApplicationPayload,
    tail?: ServiceStatefulReplyTail
  ) => boolean;
}

export interface ServiceSessionDelivery {
  readonly binding: ServiceSessionBinding;
  /**
   * Delivers the retained M6A application frame after the binding fence has
   * been validated. The stream adapter owns multipart decoding at that point.
   */
  readonly deliver: (sessionRid: string, payloadFrame: Uint8Array) => boolean;
}

interface ServiceInstanceIntent {
  readonly instanceType: string;
  readonly route: ServiceInstanceRouteFence;
}

export interface ServiceInstanceActivationReservation {
  readonly attempt: bigint;
  readonly authorityOwnerGeneration: bigint;
  readonly token: string;
}

export type ServiceInstanceApplicationIdentity = Pick<
  ServiceInstanceActivationTarget,
  'targetSpotId' | 'stableType'
>;

export type ServiceInstanceApplicationTarget = ServiceInstanceApplicationIdentity & {
  readonly objectGeneration: bigint;
};

export interface ServiceInstanceApplicationLifecycle {
  isMaterialized(target: ServiceInstanceApplicationIdentity): boolean;
  isMaterializing?(target: ServiceInstanceApplicationIdentity): boolean;
  isClosing?(target: ServiceInstanceApplicationIdentity): boolean;
  isIdleEvicting?(target: ServiceInstanceApplicationIdentity): boolean;
  beginIdleEviction?(target: ServiceInstanceApplicationIdentity): boolean;
  materialize(target: ServiceInstanceApplicationIdentity, objectGeneration: bigint): Promise<void>;
  discard(target: ServiceInstanceActivationTarget): Promise<void>;
  beginTerminal(target: ServiceInstanceApplicationTarget): void;
  completeTerminal(target: ServiceInstanceApplicationTarget): Promise<boolean>;
}

export interface ServicePendingInstanceActivation {
  readonly reservationId: string;
  readonly storeVersion: string;
  readonly objectGeneration: bigint;
  readonly authorityOwnerGeneration: bigint;
  readonly ownerId: string;
  readonly ownerLeaseGeneration: bigint;
  readonly meshName: string;
  readonly nodeRid: string;
  readonly nodeGeneration: bigint;
  readonly requestReference: string;
  readonly requestSha256: Uint8Array;
  readonly requestEncodedSize: bigint;
}

export type ServiceInstanceAuthorityRead =
  | { readonly kind: 'missing' }
  | {
      readonly kind: 'creating';
      readonly objectGeneration: bigint;
      readonly authorityOwnerGeneration: bigint;
    }
  | { readonly kind: 'ready'; readonly route: ServiceInstanceRouteFence };

export type ServiceInstanceAuthorityReserve =
  | { readonly kind: 'reserved'; readonly reservation: ServiceInstanceActivationReservation }
  | { readonly kind: 'ready'; readonly route: ServiceInstanceRouteFence };

/**
 * Synchronous authority port used by the raw ingress turn. An asynchronous
 * provider must complete outside this turn and re-enter with its result.
 */
export interface ServiceInstanceActivationAuthority {
  read(target: ServiceInstanceActivationTarget): ServiceInstanceAuthorityRead;
  reserve(
    target: ServiceInstanceActivationTarget,
    operation: { readonly high: bigint; readonly low: bigint },
    deadlineUnixMs: bigint
  ): ServiceInstanceAuthorityReserve;
  commit(
    target: ServiceInstanceActivationTarget,
    reservation: ServiceInstanceActivationReservation,
    spot: ServiceSpotState
  ): { readonly kind: 'committed' | 'lost'; readonly route: ServiceInstanceRouteFence };
  abort(target: ServiceInstanceActivationTarget, reservation: ServiceInstanceActivationReservation): void;
}

/**
 * Promise-based authority port used by real Location Store providers. Raw
 * ingress retains the envelope and resumes activation after each Store
 * operation completes; the binding callback itself never blocks.
 */
export interface ServiceAsyncInstanceActivationAuthority {
  read(target: ServiceInstanceActivationTarget): Promise<ServiceInstanceAuthorityRead>;
  reserve(
    activation: Omit<ServiceInstanceActivationRecoveryEnvelope, 'targetMeshName'>
  ): Promise<ServiceInstanceAuthorityReserve>;
  resume(
    target: ServiceInstanceActivationTarget,
    pending: ServicePendingInstanceActivation
  ): Promise<ServiceInstanceActivationReservation>;
  commit(
    target: ServiceInstanceActivationTarget,
    reservation: ServiceInstanceActivationReservation,
    spot: ServiceSpotState
  ): Promise<{ readonly kind: 'committed' | 'lost'; readonly route: ServiceInstanceRouteFence }>;
  complete(
    target: ServiceInstanceActivationTarget,
    route: ServiceInstanceRouteFence
  ): Promise<ServiceInstanceRouteFence>;
  abort(
    target: ServiceInstanceActivationTarget,
    reservation: ServiceInstanceActivationReservation
  ): Promise<void>;
}

export class ServiceInstanceActivationRedirectError extends Error {
  constructor(readonly route: ServiceInstanceRouteFence) {
    super(`Instance Spot '${route.targetSpotId}' is owned by '${route.targetNodeRid}'.`);
    this.name = 'ServiceInstanceActivationRedirectError';
  }
}

export interface ServiceUserSpotOperationResult {
  readonly terminalResult: number;
  readonly failureCode: number;
  readonly tail?: Extract<
    ServiceStatefulReplyTail,
    { readonly kind: 'userSpotCreate' | 'userSpotClose' | 'actorCreate' }
  >;
  readonly payload?: ServiceApplicationPayload;
}

export interface ServiceUserSpotOperationHandler {
  create(record: ServiceUserSpotCreateRecord, signal: AbortSignal): Promise<ServiceUserSpotOperationResult>;
  close(record: ServiceUserSpotCloseRecord, signal: AbortSignal): Promise<ServiceUserSpotOperationResult>;
  createActor?(record: ServiceActorCreateRecord, signal: AbortSignal): Promise<ServiceUserSpotOperationResult>;
}

/**
 * Owns M6B routing, identity fences and terminal operations. Application turns
 * remain in the common mailbox, where a claimed Spot or Actor owner is serial.
 */
export class ServiceStatefulRuntime {
  readonly registry: ServiceStatefulRegistry;

  private readonly operations = new ServiceTerminalOperationRegistry<ServiceStatefulResult>();
  private readonly sessionDeliveries = new Map<string, ServiceSessionDelivery>();
  private readonly subscriptions = new Map<string, Set<string>>();
  private readonly instanceIntents = new Map<string, ServiceInstanceIntent>();
  private readonly admittedInstanceOperations = new Map<string, bigint>();
  private readonly pendingInstanceTerminals = new Map<string, number>();
  private readonly pendingInstanceAuthorityTerminals = new Map<string, number>();
  private readonly instanceApplicationWaiters = new Map<string, Set<() => void>>();
  private readonly actorJoinPhases = new Map<bigint, 'admission' | 'commit' | 'abort' | undefined>();
  private readonly actorJoinTransferIds = new Map<bigint, string | undefined>();
  private readonly actorJoinCommittedTransfers = new Map<string, number>();
  private readonly actorJoinSourceLeaves = new Map<string, number>();
  private readonly actorRoutes = new Map<string, ServiceActorRouteFence>();
  // Direct application messages use the logical Spot ID. Actor joins and
  // lifecycle controls use the exact Spot object generation. Keep both views
  // so changing one routing contract cannot hide the other route fence.
  private readonly spotRoutes = new Map<string, ServiceDirectSpotRouteFence>();
  private readonly directSpotRoutes = new Map<string, ServiceDirectSpotRouteFence>();
  private readonly spotMessageFollow = new Map<string, ServiceSpotMessageFollowState>();
  private nextMessageFollowSerial = 1n;
  private nextMessageFollowOperation = 1n;
  private nextSpotId = 1n;
  private nextSessionSequence = 1n;
  private nextInstanceOperation = 1n;
  private nextUserSpotOperation = 1n;
  private instanceAuthority?: ServiceInstanceActivationAuthority;
  private asyncInstanceAuthority?: ServiceAsyncInstanceActivationAuthority;
  private instanceApplicationLifecycle?: ServiceInstanceApplicationLifecycle;
  private userSpotOperationHandler?: ServiceUserSpotOperationHandler;
  private messageFollowHandler?: (
    record: import('./service-stateful-wire-codec').ServiceMessageFollowRecord
  ) => void;
  private readonly admittedUserSpotOperations = new Map<string, {
    readonly request: string;
    readonly deadlineUnixMs: bigint;
    readonly result: Promise<ServiceUserSpotOperationResult>;
    settled: boolean;
  }>();
  private readonly pendingInstanceActivations = new Map<string, Promise<{
    readonly spot: ServiceSpotState;
    readonly route: ServiceInstanceRouteFence;
  }>>();
  private closed = false;

  constructor(
    private readonly raw: RawServiceMeshRuntime,
    readonly nodeRid: string,
    readonly nodeGeneration: bigint
  ) {
    this.registry = new ServiceStatefulRegistry(nodeRid, nodeGeneration);
    this.registry.createEntrySpot(nodeRid);
    raw.setServiceIngress(record => this.ingress(record));
  }

  createSpot(routingId?: string, kind: 'user' | 'instance' = 'user', stableType = kind): ServiceSpotState {
    this.requireOpen();
    return this.registry.createSpot(
      routingId ?? `spot-${this.nodeRid}-${this.nextSpotId++}`,
      kind,
      stableType
    );
  }

  restoreUserSpotAuthority(
    spotId: string,
    stableType: string,
    generation: bigint,
    authorityOwnerGeneration: bigint
  ): ServiceSpotState {
    this.requireOpen();
    return this.registry.restoreSpot(
      { spotId, generation },
      'user',
      stableType,
      authorityOwnerGeneration
    );
  }

  restoreSpotAuthority(
    spotId: string,
    objectKind: 'user_spot' | 'instance_spot',
    stableType: string,
    generation: bigint,
    authorityOwnerGeneration: bigint
  ): ServiceSpotState {
    this.requireOpen();
    return this.registry.restoreSpot(
      { spotId, generation },
      objectKind === 'user_spot' ? 'user' : 'instance',
      stableType,
      authorityOwnerGeneration
    );
  }

  entrySpot(): ServiceSpotState {
    return this.registry.createEntrySpot(this.nodeRid);
  }

  createActor(actorId: string, stableType = 'actor'): ServiceActorState {
    this.requireOpen();
    return this.registry.createActor(actorId, stableType);
  }

  restoreActorAuthority(
    actorId: string,
    stableType: string,
    generation: bigint,
    authorityOwnerGeneration: bigint,
    spotId: string,
    spotGeneration: bigint,
    membershipEpoch: bigint
  ): ServiceActorState {
    this.requireOpen();
    return this.registry.restoreActor(
      { nodeRid: this.nodeRid, actorId, generation },
      stableType,
      { spotId, generation: spotGeneration },
      membershipEpoch,
      authorityOwnerGeneration
    );
  }

  discardRelocatedActor(actor: ServiceActorRef): void {
    this.requireOpen();
    this.registry.destroyActor(actor);
  }

  restoreActorSessionBinding(
    actorRef: ServiceActorRef,
    sessionNodeRid: string,
    sessionRid: string,
    bindingGeneration: bigint
  ): void {
    this.requireOpen();
    const actor = this.registry.actor(actorRef.actorId);
    if (
      actor === undefined
      || actor.ref.generation !== actorRef.generation
      || actor.ref.nodeRid !== actorRef.nodeRid
    ) {
      throw new ServiceStaleGenerationError('actor', actorRef.actorId);
    }
    const current = this.registry.binding(actor.ref);
    if (
      current?.bindingGeneration === bindingGeneration
      && current.sessionOwnerNodeRid === sessionNodeRid
      && current.sessionRid === sessionRid
    ) {
      return;
    }
    this.registry.installSessionBinding({
      actor: actor.ref,
      sessionRid,
      sessionOwnerNodeRid: sessionNodeRid,
      bindingGeneration,
      membershipEpoch: actor.membershipEpoch
    });
  }

  activateInstanceSpot(
    spotId: string,
    instanceType: string,
    attempt: bigint,
    authorityOwnerGeneration = attempt
  ): { readonly spot: ServiceSpotState; readonly created: boolean } {
    this.requireOpen();
    const current = this.registry.spot(spotId);
    if (current !== undefined) {
      if (current.kind !== 'instance' || current.stableType !== instanceType) {
        throw new TypeError(`Instance Spot '${spotId}' is assigned to another type.`);
      }
      if (current.ref.generation > attempt) {
        throw new ServiceStaleGenerationError('spot', spotId);
      }
      if (current.ref.generation === attempt) {
        return {
          spot: current.authorityOwnerGeneration === authorityOwnerGeneration
            ? current
            : this.registry.restoreSpot(
                current.ref,
                'instance',
                instanceType,
                authorityOwnerGeneration
              ),
          created: false
        };
      }
      if (!this.registry.closeSpot(current.ref)) {
        throw new ServiceStaleGenerationError('spot', spotId);
      }
    }
    return {
      spot: this.registry.restoreSpot(
        { spotId, generation: attempt },
        'instance',
        instanceType,
        authorityOwnerGeneration
      ),
      created: true
    };
  }

  registerInstanceIntent(
    instanceType: string,
    route: ServiceInstanceRouteFence,
    expectedCurrentRoute?: ServiceInstanceRouteFence | null
  ): void {
    if (route.targetNodeRid !== this.nodeRid || route.targetNodeGeneration !== this.nodeGeneration) {
      throw new ServiceStaleGenerationError('spot', route.targetSpotId);
    }
    const current = this.instanceIntents.get(route.targetSpotId);
    if (current !== undefined) {
      if (current.instanceType === instanceType && sameInstanceRoute(current.route, route)) {
        return;
      }
      if (!matchesExpectedInstanceRoute(current.route, expectedCurrentRoute)) return;
      if (current.route.objectGeneration < route.objectGeneration) {
        this.instanceIntents.set(route.targetSpotId, Object.freeze({ instanceType, route: { ...route } }));
        return;
      }
      if (current.route.objectGeneration > route.objectGeneration) {
        return;
      }
      if (current.route.authorityOwnerGeneration > route.authorityOwnerGeneration) {
        return;
      }
      if (current.route.authorityOwnerGeneration === route.authorityOwnerGeneration) {
        if (!sameInstanceOwnerIdentity(current.route, route)) {
          throw new ServiceStaleGenerationError('spot', route.targetSpotId);
        }
        if (current.route.storeVersion === route.storeVersion) {
          throw new ServiceStaleGenerationError('spot', route.targetSpotId);
        }
      }
    } else if (!matchesExpectedInstanceRoute(undefined, expectedCurrentRoute)) {
      return;
    }
    this.instanceIntents.set(route.targetSpotId, Object.freeze({ instanceType, route: { ...route } }));
  }

  instanceSpotApplicationTarget(
    spotId: string
  ): { readonly stableType: string; readonly objectGeneration: bigint } | undefined {
    const intent = this.instanceIntents.get(spotId);
    const spot = this.registry.spot(spotId);
    if (
      intent === undefined
      || spot?.kind !== 'instance'
      || spot.state !== 'ready'
      || spot.ref.generation !== intent.route.objectGeneration
    ) {
      return undefined;
    }
    return {
      stableType: intent.instanceType,
      objectGeneration: intent.route.objectGeneration
    };
  }

  forgetInstanceIntent(
    spotId: string,
    objectGeneration: bigint,
    authorityOwnerGeneration: bigint,
    storeVersion?: string
  ): void {
    const current = this.instanceIntents.get(spotId);
    if (
      current?.route.objectGeneration === objectGeneration
      && current.route.authorityOwnerGeneration === authorityOwnerGeneration
      && (storeVersion === undefined || current.route.storeVersion === storeVersion)
    ) {
      this.instanceIntents.delete(spotId);
    }
  }

  registerInstanceActivationAuthority(authority: ServiceInstanceActivationAuthority): void {
    this.requireOpen();
    if (
      this.asyncInstanceAuthority !== undefined
      || this.instanceAuthority !== undefined && this.instanceAuthority !== authority
    ) {
      throw new Error('Instance activation authority is already registered.');
    }
    this.instanceAuthority = authority;
  }

  registerAsyncInstanceActivationAuthority(
    authority: ServiceAsyncInstanceActivationAuthority
  ): void {
    this.requireOpen();
    if (
      this.instanceAuthority !== undefined
      || this.asyncInstanceAuthority !== undefined
        && this.asyncInstanceAuthority !== authority
    ) {
      throw new Error('Instance activation authority is already registered.');
    }
    this.asyncInstanceAuthority = authority;
  }

  registerInstanceApplicationLifecycle(
    lifecycle: ServiceInstanceApplicationLifecycle
  ): void {
    if (this.instanceApplicationLifecycle !== undefined) {
      throw new Error('Instance application lifecycle is already registered.');
    }
    this.instanceApplicationLifecycle = lifecycle;
  }

  registerUserSpotOperationHandler(handler: ServiceUserSpotOperationHandler): void {
    this.requireOpen();
    if (
      this.userSpotOperationHandler !== undefined
      && this.userSpotOperationHandler !== handler
    ) {
      throw new Error('User Spot operation handler is already registered.');
    }
    this.userSpotOperationHandler = handler;
  }

  requestUserSpotCreate(
    targetNodeRid: string,
    request: Omit<ServiceUserSpotCreateRecord, 'kind' | 'correlation' | 'operation'>,
    timeoutMs: number
  ): Promise<ServiceUserSpotOperationResult> {
    const correlation = this.nextUserSpotOperation++;
    const operation = {
      high: this.nodeGeneration,
      low: correlation
    };
    return this.requestUserSpotOperation(
      targetNodeRid,
      encodeUserSpotCreateHeader({
        ...request,
        sourceNodeRid: this.nodeRid,
        sourceNodeGeneration: this.nodeGeneration,
        correlation,
        operation
      }),
      correlation,
      'userSpotCreate',
      timeoutMs
    );
  }

  requestUserSpotClose(
    targetNodeRid: string,
    request: Omit<ServiceUserSpotCloseRecord, 'kind' | 'correlation' | 'operation'>,
    timeoutMs: number
  ): Promise<ServiceUserSpotOperationResult> {
    const correlation = this.nextUserSpotOperation++;
    const operation = {
      high: this.nodeGeneration,
      low: correlation
    };
    return this.requestUserSpotOperation(
      targetNodeRid,
      encodeUserSpotCloseHeader({
        ...request,
        sourceNodeRid: this.nodeRid,
        sourceNodeGeneration: this.nodeGeneration,
        correlation,
        operation
      }),
      correlation,
      'userSpotClose',
      timeoutMs
    );
  }

  requestActorCreate(
    targetNodeRid: string,
    request: Omit<ServiceActorCreateRecord, 'kind' | 'correlation' | 'operation'>,
    timeoutMs: number
  ): Promise<ServiceUserSpotOperationResult> {
    const correlation = this.nextUserSpotOperation++;
    const operation = {
      high: this.nodeGeneration,
      low: correlation
    };
    return this.requestUserSpotOperation(
      targetNodeRid,
      encodeActorCreateHeader({
        ...request,
        sourceNodeRid: this.nodeRid,
        sourceNodeGeneration: this.nodeGeneration,
        correlation,
        operation
      }),
      correlation,
      'actorCreate',
      timeoutMs
    );
  }

  async recoverInstanceActivation(
    envelope: ServiceInstanceActivationRecoveryEnvelope,
    route: ServiceInstanceRouteFence,
    expectedCurrentRoute?: ServiceInstanceRouteFence | null
  ): Promise<boolean> {
    const target = envelope.target;
    if (!matchesExpectedInstanceRoute(
      this.instanceIntents.get(target.targetSpotId)?.route,
      expectedCurrentRoute
    )) return false;
    if (
      target.targetNodeRid !== this.nodeRid
      || target.targetNodeGeneration !== this.nodeGeneration
      || target.targetSpotId !== route.targetSpotId
      || route.targetNodeRid !== this.nodeRid
      || route.targetNodeGeneration !== this.nodeGeneration
    ) {
      throw new ServiceStaleGenerationError('spot', target.targetSpotId);
    }
    const authority = this.asyncInstanceAuthority;
    if (authority === undefined) {
      throw new Error('Async Instance activation authority is not registered.');
    }
    await this.instanceApplicationLifecycle?.materialize(target, route.objectGeneration);
    return this.finishRecoveredInstanceActivation(envelope, route, expectedCurrentRoute);
  }

  private async finishRecoveredInstanceActivation(
    envelope: ServiceInstanceActivationRecoveryEnvelope,
    route: ServiceInstanceRouteFence,
    expectedCurrentRoute?: ServiceInstanceRouteFence | null
  ): Promise<boolean> {
    const target = envelope.target;
    const spot = this.registry.spot(target.targetSpotId)
      ?? this.registry.restoreSpot(
        {
          spotId: target.targetSpotId,
          generation: route.objectGeneration
        },
        'instance',
        target.stableType,
        route.authorityOwnerGeneration
      );
    if (
      spot.kind !== 'instance'
      || spot.stableType !== target.stableType
      || spot.ref.generation !== route.objectGeneration
      || spot.authorityOwnerGeneration !== route.authorityOwnerGeneration
    ) {
      throw new ServiceStaleGenerationError('spot', target.targetSpotId);
    }
    if (!matchesExpectedInstanceRoute(
      this.instanceIntents.get(target.targetSpotId)?.route,
      expectedCurrentRoute
    )) return false;
    this.publishCommittedInstanceRoute(target.stableType, route, expectedCurrentRoute);
    const record = {
      kind: 'instanceSpot' as const,
      activation: 'missing' as const,
      target,
      sourceNodeGeneration: envelope.sourceNodeGeneration,
      sourceNodeRid: envelope.sourceNodeRid,
      ...(envelope.sourceSpotId === undefined
        ? {}
        : { sourceSpotId: envelope.sourceSpotId }),
      operationKind: envelope.operationKind,
      operation: envelope.operation,
      deadlineUnixMs: envelope.deadlineUnixMs,
      ...(envelope.replyRouteId === undefined ? {} : { replyRouteId: envelope.replyRouteId })
    };
    const admitted = this.enqueueActivatedInstanceSpot(
      {
        command: M6bServiceWireCommand.instanceSpot,
        flags: envelope.metadataFrame === undefined ? 0 : M6bServiceWireFlag.metadata,
        sourceRoutingId: envelope.sourceNodeRid,
        parts: [
          encodeInstanceSpotActivationHeader(
            target,
            envelope.sourceNodeGeneration,
            envelope.sourceNodeRid,
            envelope.sourceSpotId,
            envelope.operationKind,
            envelope.operation,
            envelope.deadlineUnixMs,
            envelope.replyRouteId,
            envelope.metadataFrame !== undefined
          )
        ]
      },
      record,
      Buffer.from(envelope.applicationPayloadFrame),
      spot,
      undefined,
      this.activationTerminalCompletion(target, route),
      envelope.metadataFrame === undefined
        ? undefined
        : validateServiceMetadataFrame(envelope.metadataFrame),
      true,
      this.instanceApplicationTarget(record, route.objectGeneration)
    );
    if (admitted !== 'application') {
      throw new Error('Recovered Instance activation was not admitted to the local queue.');
    }
    return true;
  }

  async recoverPendingInstanceActivation(
    envelope: ServiceInstanceActivationRecoveryEnvelope,
    pending: ServicePendingInstanceActivation,
    expectedCurrentRoute?: ServiceInstanceRouteFence | null
  ): Promise<boolean> {
    const target = envelope.target;
    if (!matchesExpectedInstanceRoute(
      this.instanceIntents.get(target.targetSpotId)?.route,
      expectedCurrentRoute
    )) return false;
    if (
      target.targetNodeRid !== this.nodeRid
      || target.targetNodeGeneration !== this.nodeGeneration
      || pending.nodeRid !== this.nodeRid
      || pending.nodeGeneration !== this.nodeGeneration
      || pending.meshName !== envelope.targetMeshName
    ) {
      throw new ServiceStaleGenerationError('spot', target.targetSpotId);
    }
    const authority = this.asyncInstanceAuthority;
    if (authority === undefined) {
      throw new Error('Async Instance activation authority is not registered.');
    }
    const reservation = await authority.resume(target, pending);
    await this.instanceApplicationLifecycle?.materialize(target, pending.objectGeneration);
    const spot = this.registry.restoreSpot(
      {
        spotId: target.targetSpotId,
        generation: pending.objectGeneration
      },
      'instance',
      target.stableType,
      pending.authorityOwnerGeneration
    );
    const committed = await authority.commit(target, reservation, spot);
    if (committed.kind !== 'committed') {
      this.registry.closeSpot(spot.ref);
      throw new ServiceInstanceActivationRedirectError(committed.route);
    }
    return this.finishRecoveredInstanceActivation(envelope, committed.route, expectedCurrentRoute);
  }

  async completeRecoveredInstanceActivation(
    target: ServiceInstanceActivationTarget,
    route: ServiceInstanceRouteFence,
    expectedCurrentRoute?: ServiceInstanceRouteFence | null
  ): Promise<ServiceInstanceRouteFence | undefined> {
    if (!matchesExpectedInstanceRoute(
      this.instanceIntents.get(target.targetSpotId)?.route,
      expectedCurrentRoute
    )) return undefined;
    const authority = this.asyncInstanceAuthority;
    if (authority === undefined) {
      throw new Error('Async Instance activation authority is not registered.');
    }
    const released = await authority.complete(target, route);
    const currentRoute = this.instanceIntents.get(target.targetSpotId)?.route;
    if (!matchesExpectedInstanceRoute(currentRoute, expectedCurrentRoute)) return undefined;
    if (currentRoute !== undefined && !sameInstanceRoute(currentRoute, route)) return undefined;
    const authorityReleased = await this.instanceApplicationLifecycle?.completeTerminal({
      targetSpotId: target.targetSpotId,
      stableType: target.stableType,
      objectGeneration: route.objectGeneration
    })
      ?? false;
    if (!authorityReleased) {
      if (currentRoute === undefined) {
        this.publishCommittedInstanceRoute(target.stableType, released, null);
      } else if (!this.replaceCommittedInstanceRoute(target.stableType, route, released)) {
          return undefined;
      }
    } else {
      this.forgetClosedInstanceRoute(released);
    }
    return released;
  }

  waitForInstanceApplicationQuiescence(
    spotId: string,
    signal?: AbortSignal
  ): Promise<void> {
    const key = String(spotId);
    if (
      (this.pendingInstanceTerminals.get(key) ?? 0) === 0
      && (this.pendingInstanceAuthorityTerminals.get(key) ?? 0) === 0
    ) {
      return Promise.resolve();
    }
    if (signal?.aborted === true) {
      return Promise.reject(signal.reason);
    }
    return new Promise<void>((resolve, reject) => {
      const waiters = this.instanceApplicationWaiters.get(key) ?? new Set<() => void>();
      this.instanceApplicationWaiters.set(key, waiters);
      const complete = () => {
        signal?.removeEventListener('abort', abort);
        resolve();
      };
      const abort = () => {
        waiters.delete(complete);
        if (waiters.size === 0) this.instanceApplicationWaiters.delete(key);
        reject(signal?.reason);
      };
      waiters.add(complete);
      signal?.addEventListener('abort', abort, { once: true });
    });
  }


  rememberActorRoute(route: ServiceActorRouteFence): void {
    if (route.actor.nodeRid.length === 0) {
      throw new TypeError('Actor route requires a target node.');
    }
    this.actorRoutes.set(actorKey(route.actor), Object.freeze({
      actor: Object.freeze({ ...route.actor }),
      targetNodeGeneration: route.targetNodeGeneration,
      authorityOwnerGeneration: route.authorityOwnerGeneration
    }));
  }

  rememberSpotRoute(
    route: ServiceDirectSpotRouteFence,
    expectedCurrentRoute?: ServiceDirectSpotRouteFence | null
  ): void {
    // Direct application messages address the logical Spot ID. Keep one
    // current owner route per ID so a same-owner reactivation can replace the
    // old object generation without making the next application message
    // stale. Message Follow and lifecycle controls keep using spotKey().
    const key = directSpotKey(route.spot.spotId);
    const current = this.directSpotRoutes.get(key);
    if (current !== undefined && sameDirectSpotRoute(current, route)) {
      this.spotRoutes.set(spotKey(route.spot), current);
      return;
    }
    if (
      expectedCurrentRoute !== undefined
      && !matchesExpectedDirectSpotRoute(current, expectedCurrentRoute)
    ) return;
    if (current !== undefined) {
      if (current.spot.generation < route.spot.generation) {
        const next = freezeDirectSpotRoute(route);
        this.directSpotRoutes.set(key, next);
        this.spotRoutes.set(spotKey(route.spot), next);
        return;
      }
      if (current.spot.generation > route.spot.generation) {
        return;
      }
      if (current.authorityOwnerGeneration > route.authorityOwnerGeneration) {
        return;
      }
      if (current.authorityOwnerGeneration === route.authorityOwnerGeneration) {
        // StoreVersion is the mutable CAS fence for the same object and
        // owner. A Preserve or membership update can advance it without
        // changing the route identity. Keep the exact fence used by the
        // latest authority projection instead of treating that update as a
        // new object generation.
        if (sameDirectSpotOwnerIdentity(current, route)) {
          const next = freezeDirectSpotRoute(route);
          this.directSpotRoutes.set(key, next);
          this.spotRoutes.set(spotKey(route.spot), next);
          return;
        }
        throw new ServiceStaleGenerationError('spot', route.spot.spotId);
      }
    }
    const next = freezeDirectSpotRoute(route);
    this.directSpotRoutes.set(key, next);
    this.spotRoutes.set(spotKey(route.spot), next);
  }

  sendMessageFollowNotification(
    targetNodeRid: string,
    record: Omit<import('./service-stateful-wire-codec').ServiceMessageFollowRecord, 'kind'>
  ): void {
    this.requireOpen();
    this.raw.sendService(targetNodeRid, [encodeMessageFollowHeader(record)]);
  }

  setMessageFollowHandler(handler: (
    record: import('./service-stateful-wire-codec').ServiceMessageFollowRecord
  ) => void): void {
    this.messageFollowHandler = handler;
  }

  sealSpotMessageFollowIngress(
    source: ServiceDirectSpotRouteFence
  ): ServiceSpotMessageFollowSeal | undefined {
    this.requireOpen();
    if (
      source.targetNodeRid !== this.nodeRid
      || source.targetNodeGeneration !== this.nodeGeneration
    ) {
      return undefined;
    }
    const key = spotKey(source.spot);
    if (this.spotMessageFollow.has(key)) return undefined;
    const seal = Object.freeze({ key, serial: this.nextMessageFollowSerial++ });
    this.spotMessageFollow.set(key, {
      seal,
      source: freezeDirectSpotRoute(source),
      queued: [],
      queuedHead: 0,
      queuedCount: 0,
      queuedBytes: 0,
      draining: false
    });
    return seal;
  }

  abortSpotMessageFollowIngress(seal: ServiceSpotMessageFollowSeal): boolean {
    const state = this.exactSpotMessageFollow(seal);
    if (state === undefined || state.target !== undefined) return false;
    this.removeSpotMessageFollow(state);
    for (let index = state.queuedHead; index < state.queued.length; index += 1) {
      const record = state.queued[index];
      if (record === undefined) continue;
      const result = this.ingress(record.ingress);
      if (result !== 'application') {
        throw new Error('Aborted Spot ingress could not be restored to its source queue.');
      }
    }
    return true;
  }

  async commitSpotMessageFollowIngress(
    seal: ServiceSpotMessageFollowSeal,
    target: ServiceDirectSpotRouteFence,
    durationMs: number
  ): Promise<boolean> {
    const state = this.exactSpotMessageFollow(seal);
    if (state === undefined || state.target !== undefined) return false;
    if (!Number.isSafeInteger(durationMs) || durationMs < 0) {
      throw new RangeError('Message Follow duration must be a non-negative safe integer.');
    }
    if (
      target.spot.spotId !== state.source.spot.spotId
      || target.spot.generation !== state.source.spot.generation
      || target.authorityOwnerGeneration <= state.source.authorityOwnerGeneration
      || target.targetNodeRid === this.nodeRid
    ) {
      throw new ServiceStaleGenerationError('spot', state.source.spot.spotId);
    }
    state.target = freezeDirectSpotRoute(target);
    state.expiresAtMs = durationMs === 0
      ? Number.MAX_SAFE_INTEGER
      : Date.now() + durationMs;
    await this.drainSpotMessageFollow(state);
    if (durationMs === 0) this.removeSpotMessageFollow(state);
    return true;
  }

  forgetSpotRoute(
    spot: ServiceSpotRef,
    authorityOwnerGeneration: bigint,
    storeVersion?: string
  ): void {
    const exactKey = spotKey(spot);
    const exact = this.spotRoutes.get(exactKey);
    const current = this.directSpotRoutes.get(directSpotKey(spot.spotId));
    if (
      exact?.authorityOwnerGeneration === authorityOwnerGeneration
      && (storeVersion === undefined || exact.storeVersion === storeVersion)
    ) {
      this.spotRoutes.delete(exactKey);
    }
    const key = directSpotKey(spot.spotId);
    if (
      current?.authorityOwnerGeneration === authorityOwnerGeneration
      && current.spot.generation === spot.generation
      && (storeVersion === undefined || current.storeVersion === storeVersion)
    ) {
      this.directSpotRoutes.delete(key);
    }
  }

  actor(actorId: string): ServiceActorState | undefined {
    return this.registry.actor(actorId);
  }

  setSubscription(spot: ServiceSpotState, channelName: string, topicFilter: string): void {
    this.registry.requireSpot(spot.ref);
    if (spot.kind === 'instance') {
      throw new Error('Instance Spots do not support logical multicast subscriptions.');
    }
    const key = subscriptionKey(channelName, topicFilter);
    const subscriptions = this.subscriptions.get(spot.ref.spotId) ?? new Set<string>();
    subscriptions.add(key);
    this.subscriptions.set(spot.ref.spotId, subscriptions);
  }

  unsetSubscription(spot: ServiceSpotState, channelName: string, topicFilter: string): void {
    const subscriptions = this.subscriptions.get(spot.ref.spotId);
    if (subscriptions === undefined) return;
    subscriptions.delete(subscriptionKey(channelName, topicFilter));
    if (subscriptions.size === 0) this.subscriptions.delete(spot.ref.spotId);
  }

  clearSubscriptions(spotId: string): void {
    this.subscriptions.delete(spotId);
  }

  publishLogicalMulticast(
    channelName: string,
    topic: string,
    payload: ServiceApplicationPayload,
    sourceSpotId = this.nodeRid
  ): void {
    this.enqueueLogicalMulticast(channelName, topic, sourceSpotId, payload);
    const targets = this.raw.topology.peers()
      .filter(peer => peer.descriptor.channels.some(
        channel => channel.name === channelName && channel.weight > 0
      ));
    const header = encodeLogicalMulticastHeader(channelName, topic, sourceSpotId);
    const payloadFrame = encodeApplicationPayload(payload);
    for (const target of targets) {
      // Remote admission ends at the source outbound transport queue. The
      // receiver's Spot queue and handler completion are not publish results.
      this.raw.sendService(target.descriptor.nodeRoutingId, [header, payloadFrame]);
    }
  }

  sendToSpot(
    sourceSpotId: string,
    requested: ServiceDirectSpotRouteFence,
    payload: ServiceApplicationPayload
  ): number {
    const target = this.acceptSpotAuthority(requested);
    if (target === undefined) {
      return SubmitResult.NotFound;
    }
    const header = encodeSpotHeader('spotSend', sourceSpotId, target);
    return this.submitOneWay(target.targetNodeRid, [header, encodeApplicationPayload(payload)]);
  }

  requestToSpot(
    sourceSpotId: string,
    requested: ServiceDirectSpotRouteFence,
    payload: ServiceApplicationPayload,
    timeoutMs: number
  ): ServiceStatefulPendingOperation {
    const pending = this.operations.reserve(timeoutMs);
    const target = this.acceptSpotAuthority(requested);
    if (target === undefined) {
      this.operations.reply(pending.id, {
        terminalResult: RequestResult.NotFound,
        failureCode: ACTOR_ROUTE_STALE
      });
      return pending;
    }
    const header = encodeSpotHeader('spotRequest', sourceSpotId, target, pending.id);
    this.submitRequest(
      pending,
      target.targetNodeRid,
      [header, encodeApplicationPayload(payload)],
      timeoutMs,
      'spotRequest'
    );
    return pending;
  }

  sendToActor(
    target: ServiceActorRef,
    _targetNodeGeneration: bigint,
    _authorityOwnerGeneration: bigint,
    payload: ServiceApplicationPayload,
    sourceActor?: ServiceActorRef,
    boundSession?: { readonly sessionRid: string; readonly bindingGeneration: bigint }
  ): number {
    const route = this.tryActorFence(target);
    if (route === undefined) return SubmitResult.NotFound;
    const header = encodeActorHeader(
      'actorSend',
      route,
      undefined,
      sourceActor,
      boundSession === undefined
        ? undefined
        : {
            ...boundSession,
            sequence: this.nextSessionSequence++
          }
    );
    return this.submitOneWay(target.nodeRid, [header, encodeApplicationPayload(payload)]);
  }

  requestToActor(
    target: ServiceActorRef,
    _targetNodeGeneration: bigint,
    _authorityOwnerGeneration: bigint,
    payload: ServiceApplicationPayload,
    timeoutMs: number,
    sourceActor?: ServiceActorRef,
    boundSession?: { readonly sessionRid: string; readonly bindingGeneration: bigint }
  ): ServiceStatefulPendingOperation {
    const pending = this.operations.reserve(timeoutMs);
    const route = this.tryActorFence(target);
    if (route === undefined) {
      this.operations.reply(pending.id, {
        terminalResult: RequestResult.NotFound,
        failureCode: ACTOR_ROUTE_STALE
      });
      return pending;
    }
    const header = encodeActorHeader(
      'actorRequest',
      route,
      pending.id,
      sourceActor,
      boundSession === undefined
        ? undefined
        : {
            ...boundSession,
            sequence: this.nextSessionSequence++
          }
    );
    this.submitRequest(
      pending,
      target.nodeRid,
      [header, encodeApplicationPayload(payload)],
      timeoutMs,
      'actorRequest'
    );
    return pending;
  }

  sendToInstanceSpot(
    route: ServiceInstanceRouteFence,
    payload: ServiceApplicationPayload,
    sourceSpotId?: string,
    metadataFrame?: Uint8Array
  ): number {
    return this.submitOneWay(route.targetNodeRid, instanceOperationParts([
      encodeInstanceSpotHeader(
        route,
        this.nodeGeneration,
        this.nodeRid,
        sourceSpotId,
        'send',
        { high: 0n, low: 0n },
        undefined,
        metadataFrame !== undefined
      ),
      encodeApplicationPayload(payload)
    ], metadataFrame));
  }

  sendToMissingInstanceSpot(
    target: ServiceInstanceActivationTarget,
    payload: ServiceApplicationPayload,
    deadlineUnixMs: bigint,
    sourceSpotId?: string,
    metadataFrame?: Uint8Array
  ): number {
    return this.prepareMissingInstanceSpotSend(
      target,
      payload,
      deadlineUnixMs,
      sourceSpotId,
      metadataFrame
    )();
  }

  sendToMissingInstanceSpotFrame(
    target: ServiceInstanceActivationTarget,
    payloadFrame: Buffer,
    deadlineUnixMs: bigint,
    sourceSpotId?: string,
    metadataFrame?: Uint8Array
  ): number {
    return this.prepareMissingInstanceSpotSendFrame(
      target,
      payloadFrame,
      deadlineUnixMs,
      sourceSpotId,
      metadataFrame
    )();
  }

  prepareMissingInstanceSpotSend(
    target: ServiceInstanceActivationTarget,
    payload: ServiceApplicationPayload,
    deadlineUnixMs: bigint,
    sourceSpotId?: string,
    metadataFrame?: Uint8Array
  ): () => number {
    const operation = { high: this.nodeGeneration, low: this.nextInstanceOperation++ };
    const parts = instanceOperationParts([
      encodeInstanceSpotActivationHeader(
        target,
        this.nodeGeneration,
        this.nodeRid,
        sourceSpotId,
        'send',
        operation,
        deadlineUnixMs,
        undefined,
        metadataFrame !== undefined
      ),
      encodeApplicationPayload(payload)
    ], metadataFrame);
    return () => this.submitOneWay(target.targetNodeRid, parts);
  }

  prepareMissingInstanceSpotSendFrame(
    target: ServiceInstanceActivationTarget,
    payloadFrame: Buffer,
    deadlineUnixMs: bigint,
    sourceSpotId?: string,
    metadataFrame?: Uint8Array
  ): () => number {
    const operation = { high: this.nodeGeneration, low: this.nextInstanceOperation++ };
    const parts = instanceOperationParts([
      encodeInstanceSpotActivationHeader(
        target,
        this.nodeGeneration,
        this.nodeRid,
        sourceSpotId,
        'send',
        operation,
        deadlineUnixMs,
        undefined,
        metadataFrame !== undefined
      ),
      payloadFrame
    ], metadataFrame);
    return () => this.submitOneWay(target.targetNodeRid, parts);
  }

  requestToInstanceSpot(
    route: ServiceInstanceRouteFence,
    payload: ServiceApplicationPayload,
    timeoutMs: number,
    sourceSpotId?: string,
    metadataFrame?: Uint8Array
  ): ServiceStatefulPendingOperation {
    const pending = this.operations.reserve(timeoutMs);
    this.submitRequest(
      pending,
      route.targetNodeRid,
      instanceOperationParts([
        encodeInstanceSpotHeader(
          route,
          this.nodeGeneration,
          this.nodeRid,
          sourceSpotId,
          'request',
          { high: 2n, low: pending.id },
          pending.id,
          metadataFrame !== undefined
        ),
        encodeApplicationPayload(payload)
      ], metadataFrame),
      timeoutMs,
      'instanceSpotRequest'
    );
    return pending;
  }

  requestToMissingInstanceSpot(
    target: ServiceInstanceActivationTarget,
    payload: ServiceApplicationPayload,
    timeoutMs: number,
    sourceSpotId?: string,
    metadataFrame?: Uint8Array
  ): ServiceStatefulPendingOperation {
    const deadlineUnixMs = BigInt(Date.now() + timeoutMs);
    const payloadFrame = encodeApplicationPayload(payload);
    return this.requestToMissingInstanceSpotFrame(
      target,
      payloadFrame,
      deadlineUnixMs,
      sourceSpotId,
      metadataFrame
    );
  }

  requestToMissingInstanceSpotFrame(
    target: ServiceInstanceActivationTarget,
    payloadFrame: Buffer,
    deadlineUnixMs: bigint,
    sourceSpotId?: string,
    metadataFrame?: Uint8Array
  ): ServiceStatefulPendingOperation {
    const timeoutMs = remainingDeadlineMs(deadlineUnixMs);
    const pending = this.operations.reserve(timeoutMs);
    this.submitRequest(
      pending,
      target.targetNodeRid,
      instanceOperationParts([
        encodeInstanceSpotActivationHeader(
          target,
          this.nodeGeneration,
          this.nodeRid,
          sourceSpotId,
          'request',
          { high: this.nodeGeneration, low: pending.id },
          deadlineUnixMs,
          pending.id,
          metadataFrame !== undefined
        ),
        payloadFrame
      ], metadataFrame),
      timeoutMs,
      'instanceSpotRequest'
    );
    return pending;
  }

  lookupRemoteActor(
    targetNodeRid: string,
    actorId: string,
    timeoutMs: number
  ): ServiceStatefulPendingOperation {
    const pending = this.operations.reserve(timeoutMs);
    this.submitRequest(
      pending,
      targetNodeRid,
      [encodeActorLookupHeader(pending.id, actorId)],
      timeoutMs,
      'actorLookup'
    );
    return pending;
  }

  destroyActor(actor: ServiceActorRef, timeoutMs: number): ServiceStatefulPendingOperation {
    const pending = this.operations.reserve(timeoutMs);
    const local = actor.nodeRid === this.nodeRid;
    if (local) {
      queueMicrotask(() => {
        try {
          this.registry.destroyActor(actor);
          this.operations.reply(pending.id, { terminalResult: RequestResult.Ok, failureCode: 0 });
        } catch (error) {
          this.operations.reply(pending.id, failure(error));
        }
      });
      return pending;
    }
    const header = encodeActorDestroyHeader(pending.id, this.actorFence(actor));
    this.submitRequest(pending, actor.nodeRid, [header], timeoutMs, 'actorDestroy');
    return pending;
  }

  joinActor(
    actor: ServiceActorRef,
    targetNodeRid: string,
    targetSpot: ServiceSpotRef,
    targetSpotGeneration: bigint,
    payload: ServiceApplicationPayload | undefined,
    timeoutMs: number
  ): ServiceStatefulPendingOperation {
    const pending = this.operations.reserve(timeoutMs);
    const target = this.trySpotFence(targetNodeRid, {
      ...targetSpot,
      generation: targetSpotGeneration
    });
    this.submitActorJoin(
      pending,
      actor,
      targetNodeRid,
      target,
      payload,
      timeoutMs
    );
    return pending;
  }

  joinActorEntrySpot(
    actor: ServiceActorRef,
    targetNodeRid: string,
    payload: ServiceApplicationPayload | undefined,
    timeoutMs: number
  ): ServiceStatefulPendingOperation {
    const pending = this.operations.reserve(timeoutMs);
    this.submitActorJoin(
      pending,
      actor,
      targetNodeRid,
      this.tryEntrySpotFence(targetNodeRid),
      payload,
      timeoutMs
    );
    return pending;
  }

  leaveActor(
    actor: ServiceActorRef,
    expectedMembershipEpoch: bigint,
    timeoutMs: number
  ): ServiceStatefulPendingOperation {
    const pending = this.operations.reserve(timeoutMs);
    queueMicrotask(() => {
      try {
        const transition = this.registry.leaveActor(actor, expectedMembershipEpoch);
        this.enqueueActorControl(transition.actor.spot.spotId, {
          kind: 'actorControl',
          lifecycleKind: ActorLifecycleKind.Left,
          previousActor: transition.actor.ref,
          currentActor: transition.actor.ref,
          previousSpotId: transition.previousSpot.spotId,
          currentSpotId: transition.currentSpot.spotId,
          previousSpotGeneration: transition.previousSpot.generation,
          currentSpotGeneration: transition.currentSpot.generation,
          previousMembershipEpoch: transition.previousMembershipEpoch,
          currentMembershipEpoch: transition.currentMembershipEpoch,
          resultCode: 0
        });
        this.operations.reply(pending.id, { terminalResult: RequestResult.Ok, failureCode: 0 });
      } catch (error) {
        this.operations.reply(pending.id, failure(error));
      }
    });
    return pending;
  }

  bindSession(
    sessionRid: string,
    actor: ServiceActorRef,
    timeoutMs: number,
    deliver: ServiceSessionDelivery['deliver']
  ): ServiceStatefulPendingOperation {
    const pending = this.operations.reserve(timeoutMs);
    const generation = this.nextSessionSequence++;
    const localBinding: ServiceSessionBinding = {
      actor,
      sessionRid,
      sessionOwnerNodeRid: this.nodeRid,
      bindingGeneration: generation,
      membershipEpoch: this.registry.actor(actor.actorId)?.membershipEpoch ?? 1n
    };
    this.sessionDeliveries.set(actorKey(actor), { binding: localBinding, deliver });
    if (actor.nodeRid === this.nodeRid) {
      try {
        const binding = this.registry.bindSession(actor, sessionRid, this.nodeRid);
        this.sessionDeliveries.set(actorKey(actor), { binding, deliver });
        this.operations.reply(pending.id, {
          terminalResult: RequestResult.Ok,
          failureCode: 0,
          kindData: undefined
        });
      } catch (error) {
        this.sessionDeliveries.delete(actorKey(actor));
        this.operations.reply(pending.id, failure(error));
      }
      return pending;
    }
    const header = encodeBoundSessionBindHeader(
      pending.id,
      this.actorFence(actor),
      sessionRid,
      { state: 'active', generation }
    );
    this.submitRequest(pending, actor.nodeRid, [header], timeoutMs, 'streamBind');
    void pending.promise.then(result => {
      if (result.terminalResult !== RequestResult.Ok) {
        this.sessionDeliveries.delete(actorKey(actor));
      }
    }, () => this.sessionDeliveries.delete(actorKey(actor)));
    return pending;
  }

  unbindSession(
    sessionRid: string,
    actor: ServiceActorRef,
    expectedBindingGeneration: bigint,
    timeoutMs: number
  ): ServiceStatefulPendingOperation {
    const pending = this.operations.reserve(timeoutMs);
    const delivery = this.sessionDeliveries.get(actorKey(actor));
    if (
      delivery === undefined
      || delivery.binding.sessionRid !== sessionRid
      || delivery.binding.bindingGeneration !== expectedBindingGeneration
    ) {
      this.operations.reply(pending.id, {
        terminalResult: RequestResult.NotFound,
        failureCode: ACTOR_ROUTE_STALE
      });
      return pending;
    }
    this.sessionDeliveries.delete(actorKey(actor));
    if (actor.nodeRid === this.nodeRid) {
      try {
        this.registry.unbindSession(actor, expectedBindingGeneration);
        this.operations.reply(pending.id, { terminalResult: RequestResult.Ok, failureCode: 0 });
      } catch (error) {
        this.operations.reply(pending.id, failure(error));
      }
      return pending;
    }
    const header = encodeBoundSessionBindHeader(
      pending.id,
      this.actorFence(actor),
      sessionRid,
      { state: 'tombstone', retiredGeneration: expectedBindingGeneration }
    );
    this.submitRequest(pending, actor.nodeRid, [header], timeoutMs, 'streamBind');
    return pending;
  }

  sessionBindings(sessionRid: string): readonly ServiceSessionBinding[] {
    return [...this.sessionDeliveries.values()]
      .map(value => value.binding)
      .filter(binding => binding.sessionRid === sessionRid);
  }

  allSessionBindings(): readonly ServiceSessionBinding[] {
    return [...this.sessionDeliveries.values()].map(value => value.binding);
  }

  sendSessionToActor(
    sessionRid: string,
    actor: ServiceActorRef,
    payload: ServiceApplicationPayload
  ): number {
    const delivery = this.sessionDeliveries.get(actorKey(actor));
    if (delivery === undefined || delivery.binding.sessionRid !== sessionRid) {
      return SubmitResult.NotFound;
    }
    return this.sendToActor(
      actor,
      this.peerGeneration(actor.nodeRid),
      actor.generation,
      payload,
      undefined,
      {
        sessionRid,
        bindingGeneration: delivery.binding.bindingGeneration
      }
    );
  }

  sendBoundSession(
    actor: ServiceActorRef,
    expectedBindingGeneration: bigint,
    payload: ServiceApplicationPayload
  ): number {
    let binding: ServiceSessionBinding;
    try {
      binding = this.registry.validateBoundSession(actor, expectedBindingGeneration);
    } catch {
      return SubmitResult.InvalidState;
    }
    const header = encodeBoundSessionSendHeader(
      this.actorFence(actor),
      expectedBindingGeneration
    );
    return this.submitOneWay(
      binding.sessionOwnerNodeRid,
      [header, encodeApplicationPayload(payload)]
    );
  }

  close(): void {
    if (this.closed) return;
    this.closed = true;
    this.operations.close();
    this.sessionDeliveries.clear();
    this.instanceIntents.clear();
    this.pendingInstanceTerminals.clear();
    this.pendingInstanceAuthorityTerminals.clear();
    for (const waiters of this.instanceApplicationWaiters.values()) {
      for (const waiter of waiters) waiter();
    }
    this.instanceApplicationWaiters.clear();
    this.admittedUserSpotOperations.clear();
    this.actorRoutes.clear();
    this.spotRoutes.clear();
    this.directSpotRoutes.clear();
    for (const state of this.spotMessageFollow.values()) {
      if (state.retryTimer !== undefined) clearTimeout(state.retryTimer);
    }
    this.spotMessageFollow.clear();
  }

  private ingress(record: RawServiceIngressRecord): RawServicePumpResult | undefined {
    if (
      record.command < M6bServiceWireCommand.spotSend
      || (
        record.command > M6bServiceWireCommand.instanceSpot
        && record.command !== M6bServiceWireCommand.userSpotCreate
        && record.command !== M6bServiceWireCommand.userSpotClose
        && record.command !== M6bServiceWireCommand.actorCreate
        && record.command !== M6bServiceWireCommand.messageFollow
      )
    ) {
      return undefined;
    }
    let decoded: ServiceStatefulWireRecord;
    try {
      decoded = decodeStatefulHeader(record.parts[0]!);
      const hasPayload = [
        'spotSend',
        'spotRequest',
        'logicalMulticast',
        'actorSend',
        'actorRequest',
        'actorJoin',
        'boundSessionSend'
      ].includes(decoded.kind);
      const instanceMetadata = decoded.kind === 'instanceSpot'
        && (record.flags & M6bServiceWireFlag.metadata) !== 0;
      const userSpotInfrastructure = decoded.kind === 'userSpotCreate'
        || decoded.kind === 'userSpotClose'
        || decoded.kind === 'actorCreate'
        || decoded.kind === 'messageFollow';
      if (
        record.parts.length < 1
        || record.parts.length > (instanceMetadata ? 3 : 2)
        || (hasPayload && decoded.kind !== 'actorJoin' && record.parts.length !== 2)
        || (
          decoded.kind === 'instanceSpot'
          && record.parts.length !== (instanceMetadata ? 3 : 2)
        )
        || (userSpotInfrastructure && record.parts.length !== 1)
      ) {
        return 'protocolError';
      }
      const metadataFrame = instanceMetadata
        ? validateServiceMetadataFrame(record.parts[1]!)
        : undefined;
      const payloadFrame = record.parts.length >= 2
        ? record.parts[record.parts.length - 1]
        : undefined;
      try {
        return this.handleIngress(record, decoded, payloadFrame, metadataFrame);
      } catch (error) {
        const correlation = statefulCorrelation(decoded);
        if (correlation !== undefined) {
          const result = failure(error);
          this.replyWire(record, correlation, result.terminalResult, result.failureCode);
          return 'infrastructure';
        }
        if (error instanceof ServiceStaleGenerationError) return 'protocolError';
        if (error instanceof ZLinkFrameworkException) {
          const kind = internalFrameworkErrorKind(error);
          if (
            kind === ZLinkFrameworkInternalErrorKind.RequestTargetNotFound
            || kind === ZLinkFrameworkInternalErrorKind.SpotMoving
            || kind === ZLinkFrameworkInternalErrorKind.SpotGenerationStale
          ) {
            return 'protocolError';
          }
        }
        throw error;
      }
    } catch (error) {
      if (error instanceof ServiceWireProtocolError) return 'protocolError';
      throw error;
    }
  }

  private handleIngress(
    ingress: RawServiceIngressRecord,
    record: ServiceStatefulWireRecord,
    payloadFrame: Buffer | undefined,
    metadataFrame?: Buffer
  ): RawServicePumpResult {
    switch (record.kind) {
      case 'messageFollow': {
        if (record.source.kind !== record.target.kind
            || record.target.authorityOwnerGeneration
              <= record.source.authorityOwnerGeneration) {
          throw new ServiceWireProtocolError(
            'Message Follow target owner generation must increase at every hop.'
          );
        }
        if (record.source.targetNodeRid !== ingress.sourceRoutingId) {
          throw new ServiceWireProtocolError(
            'Message Follow sender does not match the source route target.'
          );
        }
        const admitted = this.raw.topology.peer(record.source.targetNodeRid);
        if (admitted?.descriptor.lifecycleGeneration !== record.source.targetNodeGeneration) {
          throw new ServiceWireProtocolError(
            'Message Follow source route is not the current admitted peer.'
          );
        }
        const targetGeneration = record.target.targetNodeRid === this.nodeRid
          ? this.nodeGeneration
          : this.raw.topology.peer(record.target.targetNodeRid)?.descriptor
            .lifecycleGeneration;
        if (targetGeneration !== record.target.targetNodeGeneration) {
          throw new ServiceWireProtocolError(
            'Message Follow target route is not the current topology target.'
          );
        }
        if (record.source.kind === 'actor') {
          this.messageFollowHandler?.(record);
        } else {
          const directKey = directSpotKey(record.source.spot.spotId);
          const current = this.directSpotRoutes.get(directKey);
          if (current !== undefined
              && sameMessageFollowSpotSource(current, record.source)) {
            this.directSpotRoutes.delete(directKey);
            const exactKey = spotKey(record.source.spot);
            const exact = this.spotRoutes.get(exactKey);
            if (exact !== undefined && sameMessageFollowSpotSource(exact, record.source)) {
              this.spotRoutes.delete(exactKey);
            }
          }
        }
        return 'infrastructure';
      }
      case 'spotSend':
      case 'spotRequest': {
        const followed = this.holdOrRelaySpotMessage(ingress, record);
        if (followed !== undefined) return followed;
        this.validateDirectSpotFence(record.target);
        return this.enqueueApplicationFrame(
          ingress,
          `spot:${record.target.spot.spotId}`,
          payloadFrame!,
          {
            receiveKind: record.kind === 'spotSend' ? ReceiveKind.SpotSend : ReceiveKind.SpotRequest,
            operationKind: record.kind === 'spotRequest' ? OperationKind.SpotRequest : 0,
            ...(record.correlation === undefined ? {} : { correlation: record.correlation }),
            sourceSpotId: record.sourceSpotId,
            targetSpot: record.target.spot,
            ...(record.kind === 'spotRequest'
              ? { reply: this.replyPort(ingress, record.correlation, 'spotRequest') }
              : {})
          }
        );
      }
      case 'actorSend':
      case 'actorRequest': {
        this.validateActorFence(record.target);
        if (record.boundSession !== undefined) {
          const binding = this.registry.validateBoundSession(
            record.target.actor,
            record.boundSession.bindingGeneration
          );
          if (
            binding.sessionRid !== record.boundSession.sessionRid
            || binding.sessionOwnerNodeRid !== ingress.sourceRoutingId
          ) {
            throw new ServiceStaleGenerationError('binding', record.target.actor.actorId);
          }
        }
        return this.enqueueApplicationFrame(
          ingress,
          `actor:${record.target.actor.actorId}\0${record.target.actor.generation}`,
          payloadFrame!,
          {
            receiveKind: record.kind === 'actorSend' ? ReceiveKind.ActorSend : ReceiveKind.ActorRequest,
            operationKind: record.kind === 'actorRequest' ? OperationKind.ActorRequest : 0,
            ...(record.correlation === undefined ? {} : { correlation: record.correlation }),
            ...(record.sourceActor === undefined
              ? {}
              : { sourceActor: { ...record.sourceActor, nodeRid: ingress.sourceRoutingId } }),
            ...(record.boundSession === undefined
              ? {}
              : { sourceBindingGeneration: record.boundSession.bindingGeneration }),
            targetActor: record.target.actor,
            messageFollowOrigin: {
              sourceNodeRid: ingress.sourceRoutingId,
              originalOperation: {
                high: this.peerGeneration(ingress.sourceRoutingId),
                low: record.correlation ?? this.nextMessageFollowOperation++
              },
              originalReplyRouteId: ingress.requestSequence ?? 0n
            },
            ...(record.kind === 'actorRequest'
              ? { reply: this.replyPort(ingress, record.correlation, 'actorRequest') }
              : {})
          }
        );
      }
      case 'logicalMulticast':
        this.enqueueLogicalMulticastFrame(
          record.channelName,
          record.topic,
          record.sourceSpotId,
          payloadFrame!,
          ingress.sourceRoutingId
        );
        return 'application';
      case 'actorLookup':
        return this.replyLookup(ingress, record);
      case 'actorDestroy':
        return this.replyDestroy(ingress, record);
      case 'actorJoin':
        this.validateSpotFence(record.target);
        return this.enqueueActorJoin(ingress, record, payloadFrame);
      case 'boundSessionBind':
        return this.replySessionBind(ingress, record);
      case 'boundSessionSend':
        return this.deliverBoundSession(
          ingress,
          record,
          payloadFrame
        );
      case 'instanceSpot':
        return this.enqueueInstanceSpot(
          ingress,
          record,
          payloadFrame!,
          metadataFrame
        );
      case 'userSpotCreate':
      case 'userSpotClose':
      case 'actorCreate':
        return this.handleUserSpotOperation(ingress, record);
    }
  }

  private enqueueInstanceSpot(
    ingress: RawServiceIngressRecord,
    record: Extract<ServiceStatefulWireRecord, { readonly kind: 'instanceSpot' }>,
    payloadFrame: Buffer,
    metadataFrame?: Buffer
  ): RawServicePumpResult {
    if (record.activation === 'missing' && this.asyncInstanceAuthority !== undefined) {
      void this.continueMissingInstanceActivation(ingress, record, payloadFrame, undefined, metadataFrame);
      return 'infrastructure';
    }
    if (
      record.activation === 'ready'
      && this.needsInstanceApplicationMaterialization(ingress, record)
    ) {
      void this.continueReadyInstanceMaterialization(ingress, record, payloadFrame, metadataFrame);
      return 'infrastructure';
    }
    const spot = this.requireInstanceActivation(ingress, record);
    const applicationTarget = record.activation === 'missing'
      ? {
          targetSpotId: record.target.targetSpotId,
          stableType: record.target.stableType,
          objectGeneration: spot.ref.generation
        }
      : this.instanceApplicationTarget(record);
    return this.enqueueActivatedInstanceSpot(
      ingress,
      record,
      payloadFrame,
      spot,
      undefined,
      this.instanceApplicationTerminalCompletion(applicationTarget),
      metadataFrame
    );
  }

  private needsInstanceApplicationMaterialization(
    ingress: RawServiceIngressRecord,
    record: Extract<
      ServiceStatefulWireRecord,
      { readonly kind: 'instanceSpot'; readonly activation: 'ready' }
    >
  ): boolean {
    const lifecycle = this.instanceApplicationLifecycle;
    if (lifecycle === undefined) return false;
    const { intent } = this.requireReadyInstanceApplicationRoute(ingress, record);
    if (this.instanceApplicationLifecycle?.isIdleEvicting?.({
      targetSpotId: record.route.targetSpotId,
      stableType: intent.instanceType
    }) === true) {
      throw createInternalFrameworkException(
        ZLinkFrameworkInternalErrorKind.SpotMoving,
        `Instance Spot '${record.route.targetSpotId}' is entering idle eviction.`
      );
    }
    const applicationTarget = {
      targetSpotId: record.route.targetSpotId,
      stableType: intent.instanceType
    };
    // A close seals execution only after the current application count reaches
    // zero. A new Instance message must wait for that close even while the old
    // activation is still materialized; otherwise it can be admitted after
    // the close quiescence check and be removed with the old activation.
    if (lifecycle.isClosing?.(applicationTarget) === true) return true;
    return !lifecycle.isMaterialized(applicationTarget);
  }

  private enqueueActivatedInstanceSpot(
    ingress: RawServiceIngressRecord,
    record: Extract<ServiceStatefulWireRecord, { readonly kind: 'instanceSpot' }>,
    payloadFrame: Buffer,
    spot: ServiceSpotState,
    localReply?: NonNullable<ServiceStatefulMailboxData['reply']>,
    onTerminalCompletion?: NonNullable<ServiceStatefulMailboxData['onTerminalCompletion']>,
    metadataFrame?: Buffer,
    activationTerminal = false,
    applicationTargetOverride?: ServiceInstanceApplicationTarget
  ): RawServicePumpResult {
    let operationKey: string | undefined;
    if (record.activation === 'missing') {
      const now = BigInt(Date.now());
      for (const [key, deadline] of this.admittedInstanceOperations) {
        if (deadline < now) this.admittedInstanceOperations.delete(key);
      }
      operationKey = instanceOperationKey(record);
      if (this.admittedInstanceOperations.has(operationKey)) return 'application';
    }
    const directReply = record.operationKind === 'request'
      ? localReply ?? this.replyPort(
          ingress,
          record.replyRouteId,
          'instanceSpotRequest'
        )
      : undefined;
    const applicationTarget = applicationTargetOverride
      ?? (record.activation === 'missing'
        ? {
            targetSpotId: record.target.targetSpotId,
            stableType: record.target.stableType,
            objectGeneration: spot.ref.generation
          }
        : this.instanceApplicationTarget(record));
    let deferredReply: Parameters<NonNullable<ServiceStatefulMailboxData['reply']>> | undefined;
    const terminalCompletion = onTerminalCompletion === undefined
      ? undefined
      : async () => {
          // The application turn has returned before authority cleanup starts.
          // Mark it quiescent first so a close requested by that turn can finish
          // its local cleanup and release the exact authority fence below.
          this.completeInstanceApplicationOperation(applicationTarget.targetSpotId);
          try {
            await onTerminalCompletion();
          } finally {
            if (deferredReply !== undefined && directReply !== undefined) {
              directReply(...deferredReply);
            }
          }
        };
    const result = this.enqueueApplicationFrame(
      ingress,
      `spot:${spot.ref.spotId}`,
      payloadFrame,
      {
        receiveKind: ReceiveKind.InstanceSpotActivation,
        operationKind: record.operationKind === 'request' ? OperationKind.InstanceSpotRequest : 0,
        ...(record.operationKind === 'request' ? { correlation: record.replyRouteId } : {}),
        ...(record.sourceSpotId === undefined ? {} : { sourceSpotId: record.sourceSpotId }),
        targetSpot: spot.ref,
        ...(metadataFrame === undefined ? {} : { applicationMetadata: metadataFrame }),
        ...(terminalCompletion === undefined ? {} : { onTerminalCompletion: terminalCompletion }),
        ...(record.operationKind === 'request'
          ? {
              reply: onTerminalCompletion === undefined
                ? directReply
                : ((...reply: Parameters<NonNullable<ServiceStatefulMailboxData['reply']>>) => {
                    deferredReply = reply;
                    return true;
                  })
            }
          : {})
      }
    );
    if (operationKey !== undefined && result === 'application' && record.activation === 'missing') {
      this.admittedInstanceOperations.set(operationKey, record.deadlineUnixMs);
    }
    if (onTerminalCompletion !== undefined && result === 'application') {
      this.beginInstanceApplicationOperation(applicationTarget.targetSpotId);
      if (activationTerminal) {
        this.beginInstanceAuthorityOperation(applicationTarget.targetSpotId);
      }
      this.instanceApplicationLifecycle?.beginTerminal(applicationTarget);
    }
    return result;
  }

  private async continueMissingInstanceActivation(
    ingress: RawServiceIngressRecord,
    record: Extract<
      ServiceStatefulWireRecord,
      { readonly kind: 'instanceSpot'; readonly activation: 'missing' }
    >,
    payloadFrame: Buffer,
    localReply?: NonNullable<ServiceStatefulMailboxData['reply']>,
    metadataFrame?: Buffer
  ): Promise<void> {
    let stage = 'validate';
    statefulDebugTrace('missing-start', [
      `node=${this.nodeRid}`,
      `target=${record.target.targetSpotId}`,
      `source=${record.sourceNodeRid}`,
      `operation=${record.operation.toString()}`,
      `reply=${record.replyRouteId?.toString() ?? 'none'}`
    ]);
    try {
      this.validateInstanceIngress(ingress, record);
      stage = 'activate';
      const activation = await this.activateMissingInstanceAsync(record, payloadFrame, metadataFrame);
      statefulDebugTrace('missing-activated', [
        `node=${this.nodeRid}`,
        `target=${record.target.targetSpotId}`,
        `generation=${activation.route.objectGeneration.toString()}`,
        `ownerGeneration=${activation.route.authorityOwnerGeneration.toString()}`
      ]);
      if (this.closed) return;
      stage = 'enqueue';
      const admitted = this.enqueueActivatedInstanceSpot(
        ingress,
        record,
        payloadFrame,
        activation.spot,
        localReply,
        this.activationTerminalCompletion(record.target, activation.route),
        metadataFrame,
        true,
        this.instanceApplicationTarget(record, activation.route.objectGeneration)
      );
      if (admitted !== 'application') {
        throw new Error('Activated Instance message was not admitted to the local queue.');
      }
    } catch (error) {
      statefulDebugTrace('missing-error', [
        `node=${this.nodeRid}`,
        `target=${record.target.targetSpotId}`,
        `targetNode=${record.target.targetNodeRid}`,
        `targetGeneration=${record.target.targetNodeGeneration.toString()}`,
        `currentGeneration=${this.nodeGeneration.toString()}`,
        `sourceNode=${record.sourceNodeRid}`,
        `sourceGeneration=${record.sourceNodeGeneration.toString()}`,
        `stage=${stage}`,
        `error=${error instanceof Error ? `${error.name}:${error.message}` : String(error)}`
      ]);
      let terminalError = error;
      if (error instanceof ServiceInstanceActivationRedirectError && !this.closed) {
        try {
          await this.relayMissingInstanceActivation(
            record,
            payloadFrame,
            error.route,
            ingress,
            localReply,
            metadataFrame
          );
          return;
        } catch (relayError) {
          terminalError = relayError;
        }
      }
      if (record.operationKind !== 'request') return;
      const result = failure(terminalError);
      if (localReply !== undefined) {
        localReply(result.terminalResult, result.failureCode);
      } else {
        if (record.replyRouteId === undefined) return;
        this.replyWire(
          ingress,
          record.replyRouteId,
          result.terminalResult,
          result.failureCode
        );
      }
    }
  }

  private async continueReadyInstanceMaterialization(
    ingress: RawServiceIngressRecord,
    record: Extract<
      ServiceStatefulWireRecord,
      { readonly kind: 'instanceSpot'; readonly activation: 'ready' }
    >,
    payloadFrame: Buffer,
    metadataFrame?: Buffer
  ): Promise<void> {
    try {
      const { intent, route } = this.requireReadyInstanceApplicationRoute(ingress, record);
      const applicationTarget = {
        targetSpotId: record.route.targetSpotId,
        stableType: intent.instanceType,
        objectGeneration: route.objectGeneration
      };
      await this.instanceApplicationLifecycle?.materialize(
        applicationTarget,
        route.objectGeneration
      );
      if (this.closed) return;
      const spot = this.requireInstanceActivation(ingress, record);
      const admitted = this.enqueueActivatedInstanceSpot(
        ingress,
        record,
        payloadFrame,
        spot,
        undefined,
        this.instanceApplicationTerminalCompletion(this.instanceApplicationTarget(record)),
        metadataFrame
      );
      if (admitted !== 'application') {
        throw new Error('Rematerialized Instance message was not admitted to the local queue.');
      }
    } catch (error) {
      if (record.operationKind !== 'request' || record.replyRouteId === undefined) return;
      const result = failure(error);
      this.replyWire(
        ingress,
        record.replyRouteId,
        result.terminalResult,
        result.failureCode
      );
    }
  }

  private activationTerminalCompletion(
    target: ServiceInstanceActivationTarget,
    route: ServiceInstanceRouteFence
  ): () => Promise<void> {
    let completion: Promise<void> | undefined;
    const applicationTarget: ServiceInstanceApplicationTarget = {
      targetSpotId: target.targetSpotId,
      stableType: target.stableType,
      objectGeneration: route.objectGeneration
    };
    return () => completion ??= (async () => {
      let authorityCompletionSignaled = false;
      try {
        statefulDebugTrace('activation-completion-start', [
          `node=${this.nodeRid}`,
          `target=${target.targetSpotId}`,
          `generation=${route.objectGeneration.toString()}`,
          `ownerGeneration=${route.authorityOwnerGeneration.toString()}`
        ]);
        const released = await this.asyncInstanceAuthority?.complete(target, route);
        statefulDebugTrace('activation-completion-ok', [
          `node=${this.nodeRid}`,
          `target=${target.targetSpotId}`,
          `released=${released === undefined ? 'none' : released.storeVersion}`
        ]);
        this.completeInstanceAuthorityOperation(target.targetSpotId);
        authorityCompletionSignaled = true;
        if (released !== undefined) {
          const authorityReleased = await this.instanceApplicationLifecycle?.completeTerminal(applicationTarget)
            ?? false;
          if (!authorityReleased) {
            this.replaceCommittedInstanceRoute(target.stableType, route, released);
          } else {
            this.forgetClosedInstanceRoute(released);
          }
        }
      } catch (error) {
        statefulDebugTrace('activation-completion-error', [
          `node=${this.nodeRid}`,
          `target=${target.targetSpotId}`,
          `error=${error instanceof Error ? `${error.name}:${error.message}` : String(error)}`
        ]);
        throw error;
      } finally {
        if (!authorityCompletionSignaled) {
          this.completeInstanceAuthorityOperation(target.targetSpotId);
        }
      }
    })();
  }

  private instanceApplicationTerminalCompletion(
    target: ServiceInstanceApplicationTarget
  ): () => Promise<void> {
    return async () => {
      await this.instanceApplicationLifecycle?.completeTerminal(target);
    };
  }

  private instanceApplicationTarget(
    record: Extract<ServiceStatefulWireRecord, { readonly kind: 'instanceSpot' }>,
    missingObjectGeneration?: bigint
  ): ServiceInstanceApplicationTarget {
    if (record.activation === 'missing') {
      if (missingObjectGeneration === undefined) {
        throw new ServiceStaleGenerationError('spot', record.target.targetSpotId);
      }
      return {
        targetSpotId: record.target.targetSpotId,
        stableType: record.target.stableType,
        objectGeneration: missingObjectGeneration
      };
    }
    const intent = this.instanceIntents.get(record.route.targetSpotId);
    if (intent === undefined) {
      throw new ServiceStaleGenerationError('spot', record.route.targetSpotId);
    }
    return {
      targetSpotId: record.route.targetSpotId,
      stableType: intent.instanceType,
      objectGeneration: record.route.objectGeneration
    };
  }

  private beginInstanceApplicationOperation(spotId: string): void {
    const key = String(spotId);
    this.pendingInstanceTerminals.set(
      key,
      (this.pendingInstanceTerminals.get(key) ?? 0) + 1
    );
  }

  private beginInstanceAuthorityOperation(spotId: string): void {
    const key = String(spotId);
    this.pendingInstanceAuthorityTerminals.set(
      key,
      (this.pendingInstanceAuthorityTerminals.get(key) ?? 0) + 1
    );
  }

  private completeInstanceApplicationOperation(spotId: string): void {
    const key = String(spotId);
    const pending = this.pendingInstanceTerminals.get(key);
    if (pending === undefined || pending <= 1) {
      this.pendingInstanceTerminals.delete(key);
      this.completeInstanceApplicationWaitersIfQuiescent(key);
      return;
    }
    this.pendingInstanceTerminals.set(key, pending - 1);
  }

  private completeInstanceAuthorityOperation(spotId: string): void {
    const key = String(spotId);
    const pending = this.pendingInstanceAuthorityTerminals.get(key);
    if (pending === undefined || pending <= 1) {
      this.pendingInstanceAuthorityTerminals.delete(key);
      this.completeInstanceApplicationWaitersIfQuiescent(key);
      return;
    }
    this.pendingInstanceAuthorityTerminals.set(key, pending - 1);
  }

  private completeInstanceApplicationWaitersIfQuiescent(key: string): void {
    if (
      (this.pendingInstanceTerminals.get(key) ?? 0) !== 0
      || (this.pendingInstanceAuthorityTerminals.get(key) ?? 0) !== 0
    ) return;
    const waiters = this.instanceApplicationWaiters.get(key);
    if (waiters === undefined) return;
    this.instanceApplicationWaiters.delete(key);
    for (const waiter of waiters) waiter();
  }

  private async relayMissingInstanceActivation(
    record: Extract<
      ServiceStatefulWireRecord,
      { readonly kind: 'instanceSpot'; readonly activation: 'missing' }
    >,
    payloadFrame: Buffer,
    route: ServiceInstanceRouteFence,
    ingress: RawServiceIngressRecord,
    localReply?: NonNullable<ServiceStatefulMailboxData['reply']>,
    metadataFrame?: Buffer
  ): Promise<void> {
    const remainingMs = Number(record.deadlineUnixMs - BigInt(Date.now()));
    if (remainingMs <= 0) {
      throw new ServiceStaleGenerationError('spot', route.targetSpotId);
    }
    const redirectedTarget: ServiceInstanceActivationTarget = {
      ...record.target,
      targetNodeRid: route.targetNodeRid,
      targetNodeGeneration: route.targetNodeGeneration,
      targetSpotId: route.targetSpotId
    };
    if (record.operationKind === 'send') {
      const submitted = this.submitOneWay(route.targetNodeRid, instanceOperationParts([
        encodeInstanceSpotActivationHeader(
          redirectedTarget,
          this.nodeGeneration,
          this.nodeRid,
          record.sourceSpotId,
          'send',
          record.operation,
          record.deadlineUnixMs,
          undefined,
          metadataFrame !== undefined
        ),
        payloadFrame
      ], metadataFrame));
      if (submitted !== SubmitResult.Ok) {
        throw new Error(`Instance activation redirect was not admitted: ${submitted}.`);
      }
      return;
    }

    const pending = this.operations.reserve(remainingMs);
    this.submitRequest(
      pending,
      route.targetNodeRid,
      instanceOperationParts([
        encodeInstanceSpotActivationHeader(
          redirectedTarget,
          this.nodeGeneration,
          this.nodeRid,
          record.sourceSpotId,
          'request',
          record.operation,
          record.deadlineUnixMs,
          pending.id,
          metadataFrame !== undefined
        ),
        payloadFrame
      ], metadataFrame),
      remainingMs,
      'instanceSpotRequest'
    );
    const result = await pending.promise;
    if (localReply !== undefined) {
      localReply(result.terminalResult, result.failureCode, result.payload);
      return;
    }
    if (record.replyRouteId !== undefined) {
      this.replyWire(
        ingress,
        record.replyRouteId,
        result.terminalResult,
        result.failureCode,
        result.payload
      );
    }
  }

  private requireInstanceActivation(
    ingress: RawServiceIngressRecord,
    record: Extract<ServiceStatefulWireRecord, { readonly kind: 'instanceSpot' }>
  ): ServiceSpotState {
    const readyRoute = record.activation === 'ready' ? record.route : undefined;
    const spotId = readyRoute?.targetSpotId
      ?? (record.activation === 'missing' ? record.target.targetSpotId : undefined);
    if (spotId === undefined) throw new ServiceStaleGenerationError('spot', 'unknown');
    this.validateInstanceIngress(ingress, record);
    if (record.activation === 'missing') {
      return this.activateMissingInstance(record);
    }
    const { intent, route } = this.requireReadyInstanceApplicationRoute(ingress, record);
    const spot = this.registry.spot(route.targetSpotId)
      ?? this.registry.restoreSpot(
        {
          spotId: route.targetSpotId,
          generation: route.objectGeneration
        },
        'instance',
        intent.instanceType,
        route.authorityOwnerGeneration
      );
    if (
      spot.state !== 'ready'
      || spot.kind !== 'instance'
      || spot.stableType !== intent.instanceType
      || spot.ref.generation !== route.objectGeneration
      || spot.authorityOwnerGeneration !== route.authorityOwnerGeneration
    ) {
      throw createInternalFrameworkException(
        ZLinkFrameworkInternalErrorKind.SpotMoving,
        `Instance Spot '${route.targetSpotId}' is not projected from its current Ready authority.`
      );
    }
    return spot;
  }

  private requireReadyInstanceApplicationRoute(
    ingress: RawServiceIngressRecord,
    record: Extract<
      ServiceStatefulWireRecord,
      { readonly kind: 'instanceSpot'; readonly activation: 'ready' }
    >
  ): { readonly intent: ServiceInstanceIntent; readonly route: ServiceInstanceRouteFence } {
    this.validateInstanceIngress(ingress, record);
    let intent = this.instanceIntents.get(record.route.targetSpotId);
    if (intent === undefined) {
      // The authority commit is externally visible before the asynchronous
      // activation continuation can publish its in-memory intent. If the
      // local ready projection already carries the exact object and owner
      // fence from the received route, publish that validated projection
      // instead of exposing a transient NotFound result to a following
      // request. A mismatched or absent local projection remains NotFound.
      const local = this.registry.spot(record.route.targetSpotId);
      if (
        local?.kind === 'instance'
        && local.state === 'ready'
        && routeMatchesLocal(record.route, local, this.nodeRid, this.nodeGeneration)
      ) {
        this.publishCommittedInstanceRoute(local.stableType, record.route);
        intent = this.instanceIntents.get(record.route.targetSpotId);
      }
    }
    if (intent === undefined) {
      throw createInternalFrameworkException(
        ZLinkFrameworkInternalErrorKind.RequestTargetNotFound,
        `Instance Spot '${record.route.targetSpotId}' has no current Ready authority.`
      );
    }
    const current = intent.route;
    if (current.objectGeneration < record.route.objectGeneration) {
      throw createInternalFrameworkException(
        ZLinkFrameworkInternalErrorKind.SpotGenerationStale,
        `Instance Spot '${record.route.targetSpotId}' is behind the requested object generation.`
      );
    }
    if (!sameInstanceApplicationOwner(current, record.route)) {
      throw createInternalFrameworkException(
        ZLinkFrameworkInternalErrorKind.SpotMoving,
        `Instance Spot '${record.route.targetSpotId}' is owned by a different current route.`
      );
    }
    if (
      current.objectGeneration === record.route.objectGeneration
      && current.authorityOwnerGeneration !== record.route.authorityOwnerGeneration
    ) {
      throw createInternalFrameworkException(
        ZLinkFrameworkInternalErrorKind.SpotMoving,
        `Instance Spot '${record.route.targetSpotId}' has a different authority owner fence.`
      );
    }
    return { intent, route: current };
  }

  private validateInstanceIngress(
    ingress: RawServiceIngressRecord,
    record: Extract<ServiceStatefulWireRecord, { readonly kind: 'instanceSpot' }>
  ): void {
    const expectedSourceGeneration = this.peerGeneration(record.sourceNodeRid);
    if (
      record.sourceNodeRid !== ingress.sourceRoutingId
      || record.sourceNodeGeneration !== expectedSourceGeneration
    ) {
      if (record.activation === 'ready') {
        throw createInternalFrameworkException(
          ZLinkFrameworkInternalErrorKind.SpotMoving,
          `Instance Spot '${record.route.targetSpotId}' received a stale source lifecycle fence.`
        );
      }
      throw new ServiceStaleGenerationError('spot', record.target.targetSpotId);
    }
  }

  private activateMissingInstance(
    record: Extract<
      ServiceStatefulWireRecord,
      { readonly kind: 'instanceSpot'; readonly activation: 'missing' }
    >
  ): ServiceSpotState {
    const target = record.target;
    if (
      target.targetNodeRid !== this.nodeRid
      || target.targetNodeGeneration !== this.nodeGeneration
      || record.deadlineUnixMs < BigInt(Date.now())
    ) {
      statefulDebugTrace('missing-fence-error', [
        `node=${this.nodeRid}`,
        `target=${target.targetSpotId}`,
        `targetNode=${target.targetNodeRid}`,
        `targetGeneration=${target.targetNodeGeneration.toString()}`,
        `currentGeneration=${this.nodeGeneration.toString()}`,
        `deadline=${record.deadlineUnixMs.toString()}`,
        `now=${Date.now()}`
      ]);
      throw new ServiceStaleGenerationError('spot', target.targetSpotId);
    }
    if (this.instanceApplicationLifecycle?.isIdleEvicting?.(target) === true) {
      throw new ServiceStaleGenerationError('spot', target.targetSpotId);
    }
    const authority = this.instanceAuthority;
    if (authority === undefined) {
      throw new Error('Instance activation authority is not registered.');
    }

    const local = this.registry.spot(target.targetSpotId);
    const current = authority.read(target);
    if (local !== undefined) {
      if (
        local.kind !== 'instance'
        || local.stableType !== target.stableType
        || current.kind !== 'ready'
        || !routeMatchesLocal(current.route, local, this.nodeRid, this.nodeGeneration)
      ) {
        throw new ServiceStaleGenerationError('spot', target.targetSpotId);
      }
      return local;
    }
    if (current.kind === 'ready') {
      throw new ServiceInstanceActivationRedirectError(current.route);
    }

    const reserved = authority.reserve(target, record.operation, record.deadlineUnixMs);
    if (reserved.kind === 'ready') {
      throw new ServiceInstanceActivationRedirectError(reserved.route);
    }

    let activation: { readonly spot: ServiceSpotState; readonly created: boolean };
    try {
      activation = this.activateInstanceSpot(
        target.targetSpotId,
        target.stableType,
        reserved.reservation.attempt,
        reserved.reservation.authorityOwnerGeneration
      );
    } catch (error) {
      authority.abort(target, reserved.reservation);
      throw error;
    }
    const committed = authority.commit(target, reserved.reservation, activation.spot);
    if (committed.kind === 'lost') {
      if (activation.created) this.registry.closeSpot(activation.spot.ref);
      throw new ServiceInstanceActivationRedirectError(committed.route);
    }
    if (!routeMatchesLocal(
      committed.route,
      activation.spot,
      this.nodeRid,
      this.nodeGeneration
    )) {
      if (activation.created) this.registry.closeSpot(activation.spot.ref);
      throw new ServiceStaleGenerationError('spot', target.targetSpotId);
    }
    return activation.spot;
  }

  private activateMissingInstanceAsync(
    record: Extract<
      ServiceStatefulWireRecord,
      { readonly kind: 'instanceSpot'; readonly activation: 'missing' }
    >,
    payloadFrame: Buffer,
    metadataFrame?: Buffer
  ): Promise<{ readonly spot: ServiceSpotState; readonly route: ServiceInstanceRouteFence }> {
    const key = instanceActivationKey(record);
    const pending = this.pendingInstanceActivations.get(key);
    if (pending !== undefined) return pending;
    const activation = this.runMissingInstanceActivation(record, payloadFrame, metadataFrame);
    this.pendingInstanceActivations.set(key, activation);
    const clear = () => {
      if (this.pendingInstanceActivations.get(key) === activation) {
        this.pendingInstanceActivations.delete(key);
      }
    };
    void activation.then(clear, clear);
    return activation;
  }

  private async runMissingInstanceActivation(
    record: Extract<
      ServiceStatefulWireRecord,
      { readonly kind: 'instanceSpot'; readonly activation: 'missing' }
    >,
    payloadFrame: Buffer,
    metadataFrame?: Buffer
  ): Promise<{ readonly spot: ServiceSpotState; readonly route: ServiceInstanceRouteFence }> {
    const target = record.target;
    if (
      target.targetNodeRid !== this.nodeRid
      || target.targetNodeGeneration !== this.nodeGeneration
      || record.deadlineUnixMs < BigInt(Date.now())
    ) {
      statefulDebugTrace('missing-fence-error', [
        `node=${this.nodeRid}`,
        `target=${target.targetSpotId}`,
        `targetNode=${target.targetNodeRid}`,
        `targetGeneration=${target.targetNodeGeneration.toString()}`,
        `currentGeneration=${this.nodeGeneration.toString()}`,
        `deadline=${record.deadlineUnixMs.toString()}`,
        `now=${Date.now()}`
      ]);
      throw new ServiceStaleGenerationError('spot', target.targetSpotId);
    }
    const authority = this.asyncInstanceAuthority;
    if (authority === undefined) {
      throw new Error('Async Instance activation authority is not registered.');
    }

    const local = this.registry.spot(target.targetSpotId);
    const current = await authority.read(target);
    statefulDebugTrace('missing-state', [
      `node=${this.nodeRid}`,
      `target=${target.targetSpotId}`,
      `authority=${current.kind}`,
      `local=${local?.kind ?? 'none'}`,
      `localState=${local?.kind === 'instance' ? local.state : 'none'}`,
      `localGeneration=${local?.kind === 'instance' ? local.ref.generation.toString() : 'none'}`,
      `localOwnerGeneration=${local?.kind === 'instance' ? local.authorityOwnerGeneration.toString() : 'none'}`
    ]);
    if (current.kind === 'ready') {
      if (current.route.targetNodeRid !== this.nodeRid) {
        throw new ServiceInstanceActivationRedirectError(current.route);
      }
      if (current.route.targetNodeGeneration !== this.nodeGeneration) {
        statefulDebugTrace('missing-authority-error', [
          `node=${this.nodeRid}`,
          `target=${target.targetSpotId}`,
          `currentNode=${current.route.targetNodeRid}`,
          `currentGeneration=${current.route.targetNodeGeneration.toString()}`,
          `localGeneration=${this.nodeGeneration.toString()}`,
          `objectGeneration=${current.route.objectGeneration.toString()}`
        ]);
        throw new ServiceStaleGenerationError('spot', target.targetSpotId);
      }
      if (
        local !== undefined
        && (
          local.kind !== 'instance'
          || local.stableType !== target.stableType
        )
      ) {
        statefulDebugTrace('missing-local-error', [
          `node=${this.nodeRid}`,
          `target=${target.targetSpotId}`,
          `authority=${current.kind}`,
          `local=${local.kind}`,
          `localType=${local.kind === 'instance' ? local.stableType : 'none'}`,
          `targetType=${target.stableType}`
        ]);
        throw new ServiceStaleGenerationError('spot', target.targetSpotId);
      }
      if (
        local !== undefined
        && routeMatchesLocal(current.route, local, this.nodeRid, this.nodeGeneration)
      ) {
        return { spot: local, route: current.route };
      }
      return await this.reconcileReadyInstanceProjection(target, current.route, authority);
    }
    if (local !== undefined) {
      const lifecycle = this.instanceApplicationLifecycle;
      const closing = lifecycle?.isClosing?.(target) === true;
      const materialized = lifecycle === undefined
        ? undefined
        : lifecycle.isMaterialized(target);
      const materializing = lifecycle?.isMaterializing?.(target) === true;
      if (
        local.kind !== 'instance'
        || local.stableType !== target.stableType
      ) {
        throw new ServiceStaleGenerationError('spot', target.targetSpotId);
      }
      const joinsCreating = current.kind === 'creating'
        && local.ref.generation === current.objectGeneration
        && local.authorityOwnerGeneration === current.authorityOwnerGeneration;
      const replacesClosingProjection = current.kind === 'creating' && closing;
      // The authority can still be in a Creating reservation after the local
      // registry exposes the activation. Allow the authority reserve operation
      // to join that attempt instead of treating the local projection as stale.
      // A newer reservation can also become visible after an explicit close
      // releases authority but before local disposal removes the old
      // projection. The close gate serializes that replacement; rejecting it
      // here turns a valid next-generation Instance intent into a stale-route
      // failure.
      // A materialized projection without a close or pending materialization
      // remains stale when the authority is Missing.
      if (
        (!joinsCreating && !replacesClosingProjection && current.kind !== 'missing')
        || (!joinsCreating && !closing && materialized !== false && !materializing)
      ) {
        statefulDebugTrace('missing-projection-error', [
          `node=${this.nodeRid}`,
          `target=${target.targetSpotId}`,
          `authority=${current.kind}`,
          `closing=${closing}`,
          `materialized=${materialized === undefined ? 'none' : String(materialized)}`,
          `materializing=${materializing}`,
          `joinsCreating=${joinsCreating}`,
          `replacesClosingProjection=${replacesClosingProjection}`
        ]);
        throw new ServiceStaleGenerationError('spot', target.targetSpotId);
      }
    }
    const reserved = await authority.reserve({
      target,
      sourceNodeRid: record.sourceNodeRid,
      sourceNodeGeneration: record.sourceNodeGeneration,
      ...(record.sourceSpotId === undefined ? {} : { sourceSpotId: record.sourceSpotId }),
      operationKind: record.operationKind,
      operation: record.operation,
      ...(record.replyRouteId === undefined ? {} : { replyRouteId: record.replyRouteId }),
      deadlineUnixMs: record.deadlineUnixMs,
      ...(metadataFrame === undefined ? {} : { metadataFrame }),
      applicationPayloadFrame: payloadFrame
    });
    if (reserved.kind === 'ready') {
      throw new ServiceInstanceActivationRedirectError(reserved.route);
    }
    if (this.closed || record.deadlineUnixMs < BigInt(Date.now())) {
      await authority.abort(target, reserved.reservation);
      throw new ServiceStaleGenerationError('spot', target.targetSpotId);
    }

    let activation: { readonly spot: ServiceSpotState; readonly created: boolean };
    try {
      await this.instanceApplicationLifecycle?.materialize(target, reserved.reservation.attempt);
      activation = this.activateInstanceSpot(
        target.targetSpotId,
        target.stableType,
        reserved.reservation.attempt,
        reserved.reservation.authorityOwnerGeneration
      );
    } catch (error) {
      await authority.abort(target, reserved.reservation);
      await this.instanceApplicationLifecycle?.discard(target);
      throw error;
    }
    let committed: Awaited<ReturnType<ServiceAsyncInstanceActivationAuthority['commit']>>;
    try {
      committed = await authority.commit(target, reserved.reservation, activation.spot);
    } catch (error) {
      if (activation.created) this.registry.closeSpot(activation.spot.ref);
      await this.instanceApplicationLifecycle?.discard(target);
      throw error;
    }
    if (committed.kind === 'lost') {
      if (activation.created) this.registry.closeSpot(activation.spot.ref);
      await this.instanceApplicationLifecycle?.discard(target);
      throw new ServiceInstanceActivationRedirectError(committed.route);
    }
    if (!routeMatchesLocal(
      committed.route,
      activation.spot,
      this.nodeRid,
      this.nodeGeneration
    )) {
      if (activation.created) this.registry.closeSpot(activation.spot.ref);
      await this.instanceApplicationLifecycle?.discard(target);
      throw new ServiceStaleGenerationError('spot', target.targetSpotId);
    }
    this.publishCommittedInstanceRoute(target.stableType, committed.route);
    return { spot: activation.spot, route: committed.route };
  }

  private async reconcileReadyInstanceProjection(
    target: ServiceInstanceActivationTarget,
    route: ServiceInstanceRouteFence,
    authority: ServiceAsyncInstanceActivationAuthority
  ): Promise<{ readonly spot: ServiceSpotState; readonly route: ServiceInstanceRouteFence }> {
    await this.instanceApplicationLifecycle?.materialize(target, route.objectGeneration);
    const activation = this.activateInstanceSpot(
      target.targetSpotId,
      target.stableType,
      route.objectGeneration,
      route.authorityOwnerGeneration
    );
    if (!routeMatchesLocal(
      route,
      activation.spot,
      this.nodeRid,
      this.nodeGeneration
    )) {
      if (activation.created) this.registry.closeSpot(activation.spot.ref);
      throw new ServiceStaleGenerationError('spot', target.targetSpotId);
    }
    this.publishCommittedInstanceRoute(target.stableType, route);

    const confirmed = await authority.read(target);
    if (confirmed.kind !== 'ready' || !sameInstanceRoute(confirmed.route, route)) {
      if (
        confirmed.kind === 'ready'
        && confirmed.route.targetNodeRid !== this.nodeRid
      ) {
        throw new ServiceInstanceActivationRedirectError(confirmed.route);
      }
      throw new ServiceStaleGenerationError('spot', target.targetSpotId);
    }
    const current = this.registry.spot(target.targetSpotId);
    if (
      current === undefined
      || !routeMatchesLocal(confirmed.route, current, this.nodeRid, this.nodeGeneration)
    ) {
      throw new ServiceStaleGenerationError('spot', target.targetSpotId);
    }
    return { spot: current, route: confirmed.route };
  }

  private publishCommittedInstanceRoute(
    stableType: string,
    route: ServiceInstanceRouteFence,
    expectedCurrentRoute?: ServiceInstanceRouteFence | null
  ): void {
    const directRoute = instanceRouteAsDirectRoute(route);
    const expectedDirectRoute = expectedCurrentRoute === undefined
      ? undefined
      : expectedCurrentRoute === null
        ? null
        : instanceRouteAsDirectRoute(expectedCurrentRoute);
    this.rememberSpotRoute(directRoute, expectedDirectRoute);
    this.registerInstanceIntent(stableType, route, expectedCurrentRoute);
  }

  private replaceCommittedInstanceRoute(
    stableType: string,
    previous: ServiceInstanceRouteFence,
    next: ServiceInstanceRouteFence
  ): boolean {
    const current = this.instanceIntents.get(previous.targetSpotId);
    if (current === undefined || !sameInstanceRoute(current.route, previous)) {
      return false;
    }
    // A terminal completion can advance only the durable StoreVersion while
    // the same authority owner continues to serve the same Instance. Remove
    // the exact previous cache entry before publishing that validated result;
    // rememberSpotRoute must continue rejecting an unrelated same-generation
    // stale route.
    this.forgetSpotRoute(
      {
        spotId: previous.targetSpotId,
        generation: previous.objectGeneration
      },
      previous.authorityOwnerGeneration,
      previous.storeVersion
    );
    this.publishCommittedInstanceRoute(stableType, next, previous);
    return true;
  }

  private forgetClosedInstanceRoute(route: ServiceInstanceRouteFence): void {
    const local = this.registry.spot(route.targetSpotId);
    if (
      local?.kind === 'instance'
      && local.ref.generation === route.objectGeneration
      && local.authorityOwnerGeneration === route.authorityOwnerGeneration
    ) {
      this.registry.closeSpot(local.ref);
    }
    this.forgetSpotRoute(
      { spotId: route.targetSpotId, generation: route.objectGeneration },
      route.authorityOwnerGeneration,
      route.storeVersion
    );
    this.forgetInstanceIntent(
      route.targetSpotId,
      route.objectGeneration,
      route.authorityOwnerGeneration
    );
  }

  private enqueueActorJoin(
    ingress: RawServiceIngressRecord,
    record: Extract<ServiceStatefulWireRecord, { readonly kind: 'actorJoin' }>,
    payloadFrame: Buffer | undefined
  ): RawServicePumpResult {
    const current = this.registry.actor(record.actor.actor.actorId);
    const previousEpoch = current?.membershipEpoch ?? 0n;
    const actor: ServiceActorRef = {
      ...record.actor.actor,
      nodeRid: this.nodeRid
    };
    const control: ActorControlPayload = {
      kind: 'actorControl',
      lifecycleKind: ActorLifecycleKind.Joined,
      previousActor: current?.ref ?? record.actor.actor,
      currentActor: actor,
      previousSpotId: current?.spot.spotId ?? null,
      currentSpotId: record.target.spot.spotId,
      previousSpotGeneration: current?.spot.generation ?? 0n,
      currentSpotGeneration: record.target.spot.generation,
      previousMembershipEpoch: previousEpoch,
      currentMembershipEpoch: previousEpoch + 1n,
      resultCode: 0
    };
    const applicationFrame = payloadFrame ?? encodeApplicationPayload(emptyPayload());
    const actorJoinPhase = actorJoinPhaseFromApplicationFrame(payloadFrame);
    const actorJoinTransferId = actorJoinTransferIdFromApplicationFrame(payloadFrame);
    return this.enqueueLifecycleFrame(
      ingress,
      `spot:${record.target.spot.spotId}`,
      applicationFrame,
      {
        receiveKind: ReceiveKind.SpotControl,
        operationKind: OperationKind.ActorJoin,
        correlation: record.correlation,
        targetSpot: record.target.spot,
        targetActor: actor,
        kindData: control,
        reply: (terminalResult, failureCode, replyPayload, tail) => {
          const committedReplay = actorJoinTransferId !== undefined
            && this.hasCommittedActorJoin(actorJoinTransferId);
          if (
            actorJoinPhase !== 'admission'
            && actorJoinPhase !== 'abort'
            && !committedReplay
            && terminalResult === RequestResult.Ok
            && tail?.kind === 'actorJoin'
            && tail.joinResult === 0
          ) {
            this.commitJoinedActor(record.actor.actor, record.target.spot, previousEpoch + 1n);
            if (actorJoinTransferId !== undefined) {
              this.rememberCommittedActorJoin(actorJoinTransferId);
            }
          }
          const replyTail = committedReplay
            && tail?.kind === 'actorJoin'
            ? {
                ...tail,
                membershipEpoch: this.registry.actor(record.actor.actor.actorId)?.membershipEpoch
                  ?? tail.membershipEpoch
              }
            : tail;
          return this.replyPort(ingress, record.correlation, 'actorJoin')(
            terminalResult,
            failureCode,
            replyPayload,
            replyTail
          );
        }
      }
    );
  }

  private replyLookup(
    ingress: RawServiceIngressRecord,
    record: Extract<ServiceStatefulWireRecord, { readonly kind: 'actorLookup' }>
  ): RawServicePumpResult {
    const actor = this.registry.actor(record.actorId);
    if (actor === undefined) {
      this.replyWire(ingress, record.correlation, RequestResult.NotFound, ACTOR_ROUTE_NOT_FOUND);
    } else {
      this.replyWire(ingress, record.correlation, RequestResult.Ok, 0, undefined, {
        kind: 'actorLookup',
        actor: actor.ref,
        spot: actor.spot,
        membershipEpoch: actor.membershipEpoch,
        authorityOwnerGeneration: actor.authorityOwnerGeneration
      });
    }
    return 'infrastructure';
  }

  private replyDestroy(
    ingress: RawServiceIngressRecord,
    record: Extract<ServiceStatefulWireRecord, { readonly kind: 'actorDestroy' }>
  ): RawServicePumpResult {
    try {
      this.validateActorFence(record.actor);
      this.registry.destroyActor(record.actor.actor);
      this.replyWire(ingress, record.correlation, RequestResult.Ok, 0);
    } catch (error) {
      const result = failure(error);
      this.replyWire(ingress, record.correlation, result.terminalResult, result.failureCode);
    }
    return 'infrastructure';
  }

  private replySessionBind(
    ingress: RawServiceIngressRecord,
    record: Extract<ServiceStatefulWireRecord, { readonly kind: 'boundSessionBind' }>
  ): RawServicePumpResult {
    try {
      const actor = this.validateActorFence(record.actor);
      if (record.binding.state === 'active') {
        const binding = sessionBindingFromWire(
          actor.ref,
          record.sessionRid,
          ingress.sourceRoutingId,
          record.binding.generation,
          actor.membershipEpoch
        );
        this.registry.installSessionBinding(binding);
        this.enqueueActorBindingControl(binding);
        this.replyWire(ingress, record.correlation, RequestResult.Ok, 0, undefined, {
          kind: 'streamBind',
          bindingGeneration: binding.bindingGeneration,
          authorityOwnerGeneration: actor.authorityOwnerGeneration
        });
      } else {
        this.registry.unbindSession(
          actor.ref,
          record.binding.retiredGeneration,
          record.sessionRid,
          ingress.sourceRoutingId
        );
        this.replyWire(ingress, record.correlation, RequestResult.Ok, 0);
      }
    } catch (error) {
      const result = failure(error);
      this.replyWire(ingress, record.correlation, result.terminalResult, result.failureCode);
    }
    return 'infrastructure';
  }

  private deliverBoundSession(
    _ingress: RawServiceIngressRecord,
    record: Extract<ServiceStatefulWireRecord, { readonly kind: 'boundSessionSend' }>,
    payloadFrame: Uint8Array | undefined
  ): RawServicePumpResult {
    const delivery = this.sessionDeliveries.get(actorKey(record.actor.actor));
    if (
      delivery === undefined
      || delivery.binding.bindingGeneration !== record.expectedBindingGeneration
      || payloadFrame === undefined
      || !delivery.deliver(delivery.binding.sessionRid, payloadFrame)
    ) {
      return 'protocolError';
    }
    return 'application';
  }

  private enqueueApplicationFrame(
    ingress: RawServiceIngressRecord,
    owner: string,
    payloadFrame: Buffer,
    stateful: ServiceStatefulMailboxData
  ): RawServicePumpResult {
    return this.enqueueFrame('application', ingress, owner, payloadFrame, stateful);
  }

  private enqueueLifecycleFrame(
    ingress: RawServiceIngressRecord,
    owner: string,
    payloadFrame: Buffer,
    stateful: ServiceStatefulMailboxData
  ): RawServicePumpResult {
    return this.enqueueFrame('infrastructure', ingress, owner, payloadFrame, stateful);
  }

  private enqueueFrame(
    domain: 'application' | 'infrastructure',
    ingress: RawServiceIngressRecord,
    owner: string,
    payloadFrame: Buffer,
    stateful: ServiceStatefulMailboxData
  ): RawServicePumpResult {
    return this.raw.mailbox.tryEnqueue({
      owner,
      domain,
      parts: [ingress.parts[0]!, payloadFrame],
      sourceRoutingId: ingress.sourceRoutingId,
      sourceRoute: ingress.sourceRoute,
      ...(ingress.reply === undefined ? {} : { reply: ingress.reply }),
      ...(ingress.requestSequence === undefined ? {} : { requestSequence: ingress.requestSequence }),
      ...(stateful.correlation === undefined ? {} : { correlation: stateful.correlation }),
      stateful
    })
      ? domain
      : 'protocolError';
  }

  private enqueueLogicalMulticast(
    channelName: string,
    topic: string,
    sourceSpotId: string,
    payload: ServiceApplicationPayload,
    sourceNodeRid = this.nodeRid
  ): void {
    this.enqueueLogicalMulticastFrame(
      channelName,
      topic,
      sourceSpotId,
      encodeApplicationPayload(payload),
      sourceNodeRid
    );
  }

  private enqueueLogicalMulticastFrame(
    channelName: string,
    topic: string,
    sourceSpotId: string,
    payloadFrame: Buffer,
    sourceNodeRid = this.nodeRid
  ): void {
    const targets = [...this.subscriptions.entries()]
      .filter(([, values]) => [...values].some(value => {
        const separator = value.indexOf('\0');
        return value.slice(0, separator) === channelName
          && topicMatches(value.slice(separator + 1), topic);
      }))
      .map(([spotId]) => spotId)
      .sort();
    for (const spotId of targets) {
      const spot = this.registry.spot(spotId);
      if (spot === undefined) continue;
      this.raw.mailbox.tryEnqueue({
        owner: `spot:${spotId}`,
        domain: 'application',
        parts: [
          encodeLogicalMulticastHeader(channelName, topic, sourceSpotId),
          payloadFrame
        ],
        sourceRoutingId: sourceNodeRid,
        stateful: {
          receiveKind: ReceiveKind.SpotMulticast,
          operationKind: 0,
          sourceSpotId,
          channelName,
          topic,
          targetSpot: spot.ref
        } satisfies ServiceStatefulMailboxData
      });
    }
  }

  private enqueueActorControl(spotId: string, control: ActorControlPayload): void {
    const header = Buffer.from([0x5a, 0x4d, 1, M6bServiceWireCommand.actorJoined, 0]);
    this.raw.mailbox.tryEnqueue({
      owner: `spot:${spotId}`,
      domain: 'infrastructure',
      parts: [header],
      sourceRoutingId: this.nodeRid,
      stateful: {
        receiveKind: ReceiveKind.SpotControl,
        operationKind: 0,
        targetSpot: this.registry.spot(spotId)?.ref,
        kindData: control
      } satisfies ServiceStatefulMailboxData
    });
  }

  private enqueueActorBindingControl(
    binding: ServiceSessionBinding
  ): void {
    const actor = binding.actor;
    const header = Buffer.from([0x5a, 0x4d, 1, M6bServiceWireCommand.boundSessionBind, 0]);
    this.raw.mailbox.tryEnqueue({
      owner: `actor:${actor.actorId}\0${actor.generation}`,
      domain: 'infrastructure',
      parts: [header],
      sourceRoutingId: this.nodeRid,
      stateful: {
        receiveKind: ReceiveKind.ActorBinding,
        operationKind: 0,
        targetActor: actor,
        kindData: {
          kind: 'actorBinding',
          actor,
          bindingGeneration: binding.bindingGeneration,
          sessionNodeRid: binding.sessionOwnerNodeRid as never,
          sessionRid: binding.sessionRid as never
        }
      } satisfies ServiceStatefulMailboxData
    });
  }

  private replyPort(
    ingress: RawServiceIngressRecord,
    correlation: bigint | undefined,
    operationKind: 'spotRequest' | 'actorRequest' | 'actorJoin'
      | 'instanceSpotRequest'
  ): NonNullable<ServiceStatefulMailboxData['reply']> {
    if (correlation === undefined) throw new ServiceWireProtocolError('Request correlation is missing.');
    return (terminalResult, failureCode, payload, tail) => {
      this.replyWire(ingress, correlation, terminalResult, failureCode, payload, tail);
      void operationKind;
      return true;
    };
  }

  private replyWire(
    ingress: RawServiceIngressRecord,
    correlation: bigint,
    terminalResult: number,
    failureCode: number,
    payload?: ServiceApplicationPayload,
    tail?: ServiceStatefulReplyTail
  ): void {
    this.raw.replyService(ingress, [
      encodeStatefulReply(correlation, terminalResult, failureCode, tail),
      ...(payload === undefined ? [] : [encodeApplicationPayload(payload)])
    ]);
  }

  private handleUserSpotOperation(
    ingress: RawServiceIngressRecord,
    record: ServiceUserSpotCreateRecord | ServiceUserSpotCloseRecord | ServiceActorCreateRecord
  ): RawServicePumpResult {
    if (
      ingress.requestSequence === undefined
      || record.sourceNodeRid !== ingress.sourceRoutingId
      || record.sourceNodeGeneration !== this.peerGeneration(ingress.sourceRoutingId)
    ) {
      return 'protocolError';
    }
    const target = record.kind === 'userSpotCreate' || record.kind === 'actorCreate'
      ? record.reservation
      : record.target;
    if (
      target.targetNodeRid !== this.nodeRid
      || target.targetNodeGeneration !== this.nodeGeneration
    ) {
      this.replyWire(
        ingress,
        record.correlation,
        RequestResult.Conflict,
        SPOT_MOVING
      );
      return 'infrastructure';
    }
    const operationKey = [
      record.sourceNodeRid,
      record.sourceNodeGeneration,
      record.operation.high,
      record.operation.low
    ].join('\0');
    const encodedRequest = userSpotOperationFingerprint(record);
    let admitted = this.admittedUserSpotOperations.get(operationKey);
    if (
      admitted?.settled === true
      && operationReplayExpired(admitted.deadlineUnixMs)
    ) {
      this.admittedUserSpotOperations.delete(operationKey);
      admitted = undefined;
    }
    if (admitted !== undefined && admitted.request !== encodedRequest) {
      return 'protocolError';
    }
    if (admitted === undefined) {
      if (record.deadlineUnixMs < BigInt(Date.now())) {
        this.replyWire(
          ingress,
          record.correlation,
          RequestResult.TimedOut,
          0
        );
        return 'infrastructure';
      }
      if (this.admittedUserSpotOperations.size >= USER_SPOT_OPERATION_CAPACITY) {
        this.releaseExpiredUserSpotOperations();
      }
      if (this.admittedUserSpotOperations.size >= USER_SPOT_OPERATION_CAPACITY) {
        this.replyWire(
          ingress,
          record.correlation,
          RequestResult.Busy,
          0
        );
        return 'infrastructure';
      }
      const handler = this.userSpotOperationHandler;
      const result = handler === undefined
        ? Promise.resolve<ServiceUserSpotOperationResult>({
            terminalResult: RequestResult.InvalidState,
            failureCode: 0
          })
        : this.executeUserSpotOperation(handler, record);
      admitted = {
        request: encodedRequest,
        deadlineUnixMs: record.deadlineUnixMs,
        result,
        settled: false
      };
      this.admittedUserSpotOperations.set(operationKey, admitted);
      void result.finally(() => {
        const current = this.admittedUserSpotOperations.get(operationKey);
        if (current?.result === result) current.settled = true;
      }).catch(() => undefined);
    }
    void admitted.result.then(
      result => {
        if (this.closed) return;
        this.raw.replyService(ingress, [
          encodeStatefulReply(
            record.correlation,
            result.terminalResult,
            result.failureCode,
            result.tail
          ),
          ...(result.payload === undefined
            ? []
            : [encodeApplicationPayload(result.payload)])
        ]);
      },
      error => {
        if (this.closed) return;
        const failed = failure(error);
        this.replyWire(
          ingress,
          record.correlation,
          failed.terminalResult,
          failed.failureCode
        );
      }
    );
    return 'infrastructure';
  }

  private releaseExpiredUserSpotOperations(): void {
    for (const [key, operation] of this.admittedUserSpotOperations) {
      if (operation.settled && operationReplayExpired(operation.deadlineUnixMs)) {
        this.admittedUserSpotOperations.delete(key);
      }
    }
  }

  private async executeUserSpotOperation(
    handler: ServiceUserSpotOperationHandler,
    record: ServiceUserSpotCreateRecord | ServiceUserSpotCloseRecord | ServiceActorCreateRecord
  ): Promise<ServiceUserSpotOperationResult> {
    const deadline = userSpotDeadline(record.deadlineUnixMs);
    try {
      const result = record.kind === 'userSpotCreate'
        ? await handler.create(record, deadline.signal)
        : record.kind === 'userSpotClose'
          ? await handler.close(record, deadline.signal)
          : handler.createActor === undefined
            ? {
                terminalResult: RequestResult.InvalidState,
                failureCode: 0
              }
            : await handler.createActor(record, deadline.signal);
      if (
        result.terminalResult === RequestResult.Ok
        && (
          result.tail === undefined
          || result.tail.kind !== record.kind
        )
      ) {
        throw new ServiceWireProtocolError('User Spot success result omits its operation tail.');
      }
      return result;
    } catch (error) {
      const failed = failure(error);
      return failed;
    } finally {
      deadline.close();
    }
  }

  private async requestUserSpotOperation(
    targetNodeRid: string,
    header: Buffer,
    correlation: bigint,
    operationKind: 'userSpotCreate' | 'userSpotClose' | 'actorCreate',
    timeoutMs: number
  ): Promise<ServiceUserSpotOperationResult> {
    this.requireOpen();
    const deadlineMs = Date.now() + Math.max(1, timeoutMs);
    let parts: readonly Buffer[];
    for (;;) {
      const remainingMs = deadlineMs - Date.now();
      if (remainingMs <= 0) {
        throw new Error(`${operationKind} timed out before terminal replay.`);
      }
      try {
        parts = targetNodeRid === this.nodeRid
          ? await this.requestLocalInfrastructure(header, correlation, remainingMs)
          : await this.raw.requestService(targetNodeRid, [header], remainingMs);
        break;
      } catch (error) {
        const retryDelayMs = Math.min(20, deadlineMs - Date.now());
        if (retryDelayMs <= 0) throw error;
        await new Promise(resolve => setTimeout(resolve, retryDelayMs));
      }
    }
    if (parts.length < 1 || parts.length > 2) {
      throw new ServiceWireProtocolError('Invalid creation operation reply parts.');
    }
    const decoded = decodeStatefulReply(parts[0]!, correlation, operationKind, parts.length >= 2);
    if (
      decoded.terminalResult !== RequestResult.Ok
      && parts.length !== 1
    ) {
      throw new ServiceWireProtocolError('Failed creation operation reply carries a payload.');
    }
    if (
      operationKind === 'userSpotClose'
      && parts.length !== 1
    ) {
      throw new ServiceWireProtocolError('User Spot close reply carries a payload.');
    }
    return {
      terminalResult: decoded.terminalResult,
      failureCode: decoded.failureCode,
      ...(decoded.tail === undefined ? {} : { tail: decoded.tail as never }),
      ...(parts.length === 2
        ? { payload: decodeApplicationPayload(parts[1]!) }
        : {})
    };
  }

  private requestLocalInfrastructure(
    header: Buffer,
    requestSequence: bigint,
    timeoutMs: number
  ): Promise<readonly Buffer[]> {
    return new Promise((resolve, reject) => {
      let settled = false;
      const timer = setTimeout(() => {
        if (settled) return;
        settled = true;
        reject(new Error('Local infrastructure request timed out.'));
      }, timeoutMs);
      const finish = (parts: readonly Uint8Array[]) => {
        if (settled) return;
        settled = true;
        clearTimeout(timer);
        resolve(parts.map(part => Buffer.from(part)));
      };
      try {
        const result = this.ingress({
          command: header[3]!,
          flags: header[4]!,
          sourceRoutingId: this.nodeRid,
          requestSequence,
          reply: finish,
          parts: [header]
        });
        if (result !== 'infrastructure') {
          clearTimeout(timer);
          settled = true;
          reject(new ServiceWireProtocolError(
            `Local infrastructure request was rejected as '${result ?? 'unsupported'}'.`
          ));
        }
      } catch (error) {
        clearTimeout(timer);
        settled = true;
        reject(error);
      }
    });
  }

  private submitOneWay(targetNodeRid: string, parts: readonly Buffer[]): number {
    this.requireOpen();
    if (targetNodeRid === this.nodeRid) {
      const result = this.ingress({
        command: parts[0]![3]!,
        flags: parts[0]![4]!,
        sourceRoutingId: this.nodeRid,
        parts
      });
      return result === 'application' || result === 'infrastructure'
        ? SubmitResult.Ok
        : SubmitResult.InvalidState;
    }
    if (this.raw.sendService(targetNodeRid, parts)) return SubmitResult.Ok;
    return rawPeerRouteReady(this.raw, targetNodeRid) === true
      ? SubmitResult.Backpressured
      : SubmitResult.NotConnected;
  }

  private submitRequest(
    pending: ServiceStatefulPendingOperation,
    targetNodeRid: string,
    parts: readonly Buffer[],
    timeoutMs: number,
    operationKind: 'spotRequest' | 'actorRequest' | 'actorLookup' | 'actorDestroy' | 'actorJoin' | 'streamBind'
      | 'instanceSpotRequest',
    actor?: ServiceActorRef
  ): void {
    if (targetNodeRid === this.nodeRid) {
      const deadlineUnixMs = BigInt(Date.now() + Math.max(0, timeoutMs));
      const localIngress: RawServiceIngressRecord = {
        command: parts[0]![3]!,
        flags: parts[0]![4]!,
        sourceRoutingId: this.nodeRid,
        requestSequence: pending.id,
        parts
      };
      try {
        this.submitLocalRequest(
          localIngress,
          pending,
          operationKind,
          actor,
          deadlineUnixMs
        );
      } catch (error) {
        this.operations.reply(pending.id, failure(error));
      }
      return;
    }
    void this.raw.requestService(targetNodeRid, parts, timeoutMs).then(
      reply => this.completeRemoteReply(pending, targetNodeRid, operationKind, actor, reply),
      error => {
        if (operationKind === 'actorJoin') {
          this.actorJoinPhases.delete(pending.id);
          this.actorJoinTransferIds.delete(pending.id);
        }
        this.operations.fail(pending.id, error);
      }
    );
  }

  private submitActorJoin(
    pending: ServiceStatefulPendingOperation,
    actor: ServiceActorRef,
    targetNodeRid: string,
    target: ServiceSpotRouteFence | undefined,
    payload: ServiceApplicationPayload | undefined,
    timeoutMs: number
  ): void {
    const actorRoute = this.tryActorFence(actor);
    if (target === undefined || actorRoute === undefined) {
      this.operations.reply(pending.id, {
        terminalResult: RequestResult.NotFound,
        failureCode: ACTOR_ROUTE_STALE
      });
      return;
    }
    const header = encodeActorJoinHeader(
      pending.id,
      actorRoute,
      target.spot.spotId === targetNodeRid,
      target
    );
    this.actorJoinPhases.set(
      pending.id,
      actorJoinMetadataFromMultipartPayload(payload?.payload)?.phase
    );
    this.actorJoinTransferIds.set(
      pending.id,
      actorJoinMetadataFromMultipartPayload(payload?.payload)?.transferId
    );
    this.submitRequest(
      pending,
      targetNodeRid,
      [
        header,
        ...(payload === undefined ? [] : [encodeApplicationPayload(payload)])
      ],
      timeoutMs,
      'actorJoin',
      actor
    );
  }

  private submitLocalRequest(
    ingress: RawServiceIngressRecord,
    pending: ServiceStatefulPendingOperation,
    operationKind: 'spotRequest' | 'actorRequest' | 'actorLookup' | 'actorDestroy' | 'actorJoin' | 'streamBind'
      | 'instanceSpotRequest',
    actor?: ServiceActorRef,
    deadlineUnixMs?: bigint
  ): void {
    const decoded = decodeStatefulHeader(ingress.parts[0]!);
    const payloadFrame = ingress.parts.length < 2
      ? undefined
      : ingress.parts[1];
    if (decoded.kind === 'actorLookup') {
      const found = this.registry.actor(decoded.actorId);
      this.operations.reply(pending.id, found === undefined
        ? { terminalResult: RequestResult.NotFound, failureCode: ACTOR_ROUTE_NOT_FOUND }
        : {
            terminalResult: RequestResult.Ok,
            failureCode: 0,
            kindData: lookupKindData(found)
          });
      return;
    }
    if (decoded.kind === 'actorDestroy') {
      try {
        this.registry.destroyActor(decoded.actor.actor);
        this.operations.reply(pending.id, { terminalResult: RequestResult.Ok, failureCode: 0 });
      } catch (error) {
        this.operations.reply(pending.id, failure(error));
      }
      return;
    }
    const localReply: ServiceStatefulMailboxData['reply'] = (
      terminalResult,
      failureCode,
      replyPayload,
      tail
    ) => {
      statefulDebugTrace('local-reply', [
        `node=${this.nodeRid}`,
        `pending=${pending.id.toString()}`,
        `kind=${decoded.kind}`,
        `result=${terminalResult}`,
        `failure=${failureCode}`,
        `tail=${tail?.kind ?? 'none'}`
      ]);
      return this.operations.reply(pending.id, this.resultFromReply(
        terminalResult,
        failureCode,
        replyPayload,
        tail,
        this.nodeRid,
        actor
      ));
    };
    if (decoded.kind === 'spotRequest') {
      this.validateDirectSpotFence(decoded.target);
      this.enqueueApplicationFrame(
        ingress,
        `spot:${decoded.target.spot.spotId}`,
        payloadFrame!,
        {
          receiveKind: ReceiveKind.SpotRequest,
          operationKind: OperationKind.SpotRequest,
          correlation: decoded.correlation,
          sourceSpotId: decoded.sourceSpotId,
          targetSpot: decoded.target.spot,
          reply: localReply
        }
      );
      return;
    }
    if (decoded.kind === 'actorRequest') {
      this.validateActorFence(decoded.target);
      this.enqueueApplicationFrame(
        ingress,
        `actor:${decoded.target.actor.actorId}\0${decoded.target.actor.generation}`,
        payloadFrame!,
        {
          receiveKind: ReceiveKind.ActorRequest,
          operationKind: OperationKind.ActorRequest,
          correlation: decoded.correlation,
          targetActor: decoded.target.actor,
          ...(decoded.sourceActor === undefined ? {} : { sourceActor: decoded.sourceActor }),
          ...(decoded.boundSession === undefined
            ? {}
            : { sourceBindingGeneration: decoded.boundSession.bindingGeneration }),
          reply: localReply
        }
      );
      return;
    }
    if (decoded.kind === 'actorJoin') {
      this.validateSpotFence(decoded.target);
      const previous = this.registry.actor(decoded.actor.actor.actorId);
      const control: ActorControlPayload = {
        kind: 'actorControl',
        lifecycleKind: ActorLifecycleKind.Joined,
        previousActor: previous?.ref ?? decoded.actor.actor,
        currentActor: decoded.actor.actor,
        previousSpotId: previous?.spot.spotId ?? null,
        currentSpotId: decoded.target.spot.spotId,
        previousSpotGeneration: previous?.spot.generation ?? 0n,
        currentSpotGeneration: decoded.target.spot.generation,
        previousMembershipEpoch: previous?.membershipEpoch ?? 0n,
        currentMembershipEpoch: (previous?.membershipEpoch ?? 0n) + 1n,
        resultCode: 0
      };
      this.enqueueLifecycleFrame(
        ingress,
        `spot:${decoded.target.spot.spotId}`,
        payloadFrame ?? encodeApplicationPayload(emptyPayload()),
        {
          receiveKind: ReceiveKind.SpotControl,
          operationKind: OperationKind.ActorJoin,
          correlation: decoded.correlation,
          targetSpot: decoded.target.spot,
          targetActor: decoded.actor.actor,
          kindData: control,
          isPending: () => this.operations.isPending(pending.id),
          ...(deadlineUnixMs === undefined ? {} : { deadlineUnixMs }),
          reply: (terminalResult, failureCode, replyPayload, tail) => {
            const actorJoinTransferId = actorJoinTransferIdFromApplicationFrame(payloadFrame);
            const committedReplay = actorJoinTransferId !== undefined
              && this.hasCommittedActorJoin(actorJoinTransferId);
            const replyTail = committedReplay
              && tail?.kind === 'actorJoin'
              ? {
                  ...tail,
                  membershipEpoch: this.registry.actor(decoded.actor.actor.actorId)?.membershipEpoch
                    ?? tail.membershipEpoch
                }
              : tail;
            const acceptedReply = localReply(terminalResult, failureCode, replyPayload, replyTail);
            if (
              acceptedReply
              &&
              !committedReplay
              && terminalResult === RequestResult.Ok
              && tail?.kind === 'actorJoin'
              && tail.joinResult === 0
            ) {
              this.commitJoinedActor(
                decoded.actor.actor,
                decoded.target.spot,
                control.currentMembershipEpoch
              );
              if (actorJoinTransferId !== undefined) {
                this.rememberCommittedActorJoin(actorJoinTransferId);
              }
            }
            return acceptedReply;
          }
        }
      );
      return;
    }
    if (decoded.kind === 'boundSessionBind') {
      try {
        const current = this.validateActorFence(decoded.actor);
        if (decoded.binding.state === 'active') {
          this.registry.installSessionBinding(sessionBindingFromWire(
            current.ref,
            decoded.sessionRid,
            this.nodeRid,
            decoded.binding.generation,
            current.membershipEpoch
          ));
        } else {
          this.registry.unbindSession(
            current.ref,
            decoded.binding.retiredGeneration,
            decoded.sessionRid,
            this.nodeRid
          );
        }
        this.operations.reply(pending.id, { terminalResult: RequestResult.Ok, failureCode: 0 });
      } catch (error) {
        this.operations.reply(pending.id, failure(error));
      }
      return;
    }
    if (decoded.kind === 'instanceSpot') {
      if (decoded.activation === 'missing' && this.asyncInstanceAuthority !== undefined) {
        void this.continueMissingInstanceActivation(
          ingress,
          decoded,
          payloadFrame!,
          localReply
        );
        return;
      }
      try {
        const activation = this.requireInstanceActivation(ingress, decoded);
        this.enqueueApplicationFrame(
          ingress,
          `spot:${activation.ref.spotId}`,
          payloadFrame!,
          {
            receiveKind: ReceiveKind.InstanceSpotActivation,
            operationKind: OperationKind.InstanceSpotRequest,
            correlation: pending.id,
            ...(decoded.sourceSpotId === undefined ? {} : { sourceSpotId: decoded.sourceSpotId }),
            targetSpot: activation.ref,
            reply: localReply
          }
        );
      } catch (error) {
        this.operations.reply(pending.id, failure(error));
      }
      return;
    }
    this.operations.reply(pending.id, {
      terminalResult: RequestResult.ProtocolError,
      failureCode: 0
    });
    void operationKind;
  }

  private completeRemoteReply(
    pending: ServiceStatefulPendingOperation,
    targetNodeRid: string,
    operationKind: 'spotRequest' | 'actorRequest' | 'actorLookup' | 'actorDestroy' | 'actorJoin' | 'streamBind'
      | 'instanceSpotRequest',
    actor: ServiceActorRef | undefined,
    reply: readonly Buffer[]
  ): void {
    const actorJoinPhase = operationKind === 'actorJoin'
      ? this.actorJoinPhases.get(pending.id)
      : undefined;
    const actorJoinTransferId = operationKind === 'actorJoin'
      ? this.actorJoinTransferIds.get(pending.id)
      : undefined;
    try {
      if (reply.length < 1 || reply.length > 2) throw new ServiceWireProtocolError('Invalid M6B reply parts.');
      const decoded = decodeStatefulReply(reply[0]!, pending.id, operationKind, reply.length >= 2);
      if (decoded.terminalResult !== RequestResult.Ok || decoded.failureCode !== 0) {
        statefulDebugTrace('remote-reply-error', [
          `node=${this.nodeRid}`,
          `pending=${pending.id.toString()}`,
          `target=${targetNodeRid}`,
          `kind=${operationKind}`,
          `result=${decoded.terminalResult}`,
          `failure=${decoded.failureCode}`,
          `tail=${decoded.tail?.kind ?? 'none'}`
        ]);
      }
      const payload = reply.length < 2 ? undefined : decodeApplicationPayload(reply[1]);
      if (
        operationKind === 'actorJoin'
        && actor !== undefined
        && actorJoinPhase !== 'admission'
        && actorJoinPhase !== 'abort'
        && decoded.terminalResult === RequestResult.Ok
        && decoded.tail?.kind === 'actorJoin'
        && decoded.tail.joinResult === 0
        && (
          actorJoinTransferId === undefined
          || !this.hasSourceLeave(actorJoinTransferId)
        )
      ) {
        this.enqueueRemoteSourceLeave(actor);
        if (actorJoinTransferId !== undefined) {
          this.rememberSourceLeave(actorJoinTransferId);
        }
      }
      this.operations.reply(pending.id, this.resultFromReply(
        decoded.terminalResult,
        decoded.failureCode,
        payload,
        decoded.tail,
        targetNodeRid,
        actor
      ));
    } catch (error) {
      this.operations.fail(pending.id, error);
    } finally {
      if (operationKind === 'actorJoin') {
        this.actorJoinPhases.delete(pending.id);
        this.actorJoinTransferIds.delete(pending.id);
      }
    }
  }

  private enqueueRemoteSourceLeave(actor: ServiceActorRef): void {
    const current = this.registry.requireActor(actor);
    const transition = this.registry.leaveActor(actor, current.membershipEpoch);
    // The Core-owned remote Entry transfer emits the LEFT lifecycle event to
    // the source membership. The actor state already points at the Entry
    // Spot, so routing this control to currentSpot would skip the source
    // User Spot's onLeaveActor callback and only notify the Entry callback.
    this.enqueueActorControl(transition.previousSpot.spotId, {
      kind: 'actorControl',
      lifecycleKind: ActorLifecycleKind.Left,
      previousActor: transition.actor.ref,
      currentActor: transition.actor.ref,
      previousSpotId: transition.previousSpot.spotId,
      currentSpotId: transition.currentSpot.spotId,
      previousSpotGeneration: transition.previousSpot.generation,
      currentSpotGeneration: transition.currentSpot.generation,
      previousMembershipEpoch: transition.previousMembershipEpoch,
      currentMembershipEpoch: transition.currentMembershipEpoch,
      resultCode: 0
    });
  }

  private resultFromReply(
    terminalResult: number,
    failureCode: number,
    payload: ServiceApplicationPayload | undefined,
    tail: ServiceStatefulReplyTail | undefined,
    targetNodeRid: string,
    actor: ServiceActorRef | undefined
  ): ServiceStatefulResult {
    let kindData: ReceiveKindData | undefined;
    if (tail?.kind === 'actorLookup') {
      const resolvedActor = { ...tail.actor, nodeRid: targetNodeRid };
      this.rememberActorRoute({
        actor: resolvedActor,
        targetNodeGeneration: this.peerGeneration(targetNodeRid),
        authorityOwnerGeneration: tail.authorityOwnerGeneration
      });
      kindData = {
        kind: 'actorLookupCompletion',
        location: {
          actor: resolvedActor,
          spotId: tail.spot.spotId,
          spotGeneration: tail.spot.generation,
          membershipEpoch: tail.membershipEpoch
        }
      };
    } else if (tail?.kind === 'actorJoin' && actor !== undefined) {
      const joinedActor = { ...actor, nodeRid: targetNodeRid };
      const spot = tail.spot;
      kindData = {
        kind: 'actorJoinCompletion',
        joinResult: tail.joinResult,
        actor: joinedActor,
        location: {
          actor: joinedActor,
          spotId: spot?.spotId ?? null,
          spotGeneration: spot?.generation ?? 0n,
          membershipEpoch: tail.membershipEpoch ?? 1n
        }
      } satisfies ActorJoinCompletionPayload;
    }
    return {
      terminalResult,
      failureCode,
      ...(payload === undefined ? {} : { payload }),
      ...(kindData === undefined ? {} : { kindData })
    };
  }

  private validateActorFence(fence: ServiceActorRouteFence): ServiceActorState {
    if (fence.actor.nodeRid !== this.nodeRid || fence.targetNodeGeneration !== this.nodeGeneration) {
      throw new ServiceStaleGenerationError('actor', fence.actor.actorId);
    }
    const actor = this.registry.requireActor(fence.actor);
    if (actor.authorityOwnerGeneration !== fence.authorityOwnerGeneration) {
      throw new ServiceStaleGenerationError('actor', fence.actor.actorId);
    }
    return actor;
  }

  private validateSpotFence(fence: ServiceSpotRouteFence): ServiceSpotState {
    if (fence.targetNodeRid !== this.nodeRid || fence.targetNodeGeneration !== this.nodeGeneration) {
      throw new ServiceStaleGenerationError('spot', fence.spot.spotId);
    }
    const spot = this.registry.requireSpot(fence.spot);
    if (spot.authorityOwnerGeneration !== fence.authorityOwnerGeneration) {
      throw new ServiceStaleGenerationError('spot', fence.spot.spotId);
    }
    return spot;
  }

  private validateDirectSpotFence(fence: ServiceDirectSpotRouteFence): ServiceSpotState {
    const spot = this.registry.spot(fence.spot.spotId);
    if (spot === undefined) {
      throw createInternalFrameworkException(
        ZLinkFrameworkInternalErrorKind.RequestTargetNotFound,
        `Spot '${fence.spot.spotId}' has no current Ready authority.`
      );
    }
    if (spot.state !== 'ready') {
      throw createInternalFrameworkException(
        ZLinkFrameworkInternalErrorKind.SpotMoving,
        `Spot '${fence.spot.spotId}' is not Ready.`
      );
    }
    if (fence.targetNodeRid !== this.nodeRid || fence.targetNodeGeneration !== this.nodeGeneration) {
      throw createInternalFrameworkException(
        ZLinkFrameworkInternalErrorKind.SpotMoving,
        `Spot '${fence.spot.spotId}' targets an unavailable owner route.`
      );
    }
    if (spot.kind === 'entry') {
      if (!isEntrySpotFence(fence)) {
        throw createInternalFrameworkException(
          ZLinkFrameworkInternalErrorKind.SpotMoving,
          `Entry Spot '${fence.spot.spotId}' has an invalid owner fence.`
        );
      }
      return spot;
    }
    const current = this.directSpotRoutes.get(directSpotKey(spot.ref.spotId));
    if (current === undefined) {
      throw createInternalFrameworkException(
        ZLinkFrameworkInternalErrorKind.SpotMoving,
        `Spot '${fence.spot.spotId}' has no current owner route.`
      );
    }
    if (
      current.targetNodeRid !== this.nodeRid
      || current.targetNodeGeneration !== this.nodeGeneration
      || current.ownerLeaseGeneration !== fence.ownerLeaseGeneration
      || (
        current.spot.generation === fence.spot.generation
        && (
          current.authorityOwnerGeneration !== fence.authorityOwnerGeneration
        )
      )
    ) {
      throw createInternalFrameworkException(
        ZLinkFrameworkInternalErrorKind.SpotMoving,
        `Spot '${fence.spot.spotId}' has a stale owner fence.`
      );
    }
    return spot;
  }

  private holdOrRelaySpotMessage(
    ingress: RawServiceIngressRecord,
    wire: Extract<
      ServiceStatefulWireRecord,
      { readonly kind: 'spotSend' | 'spotRequest' }
    >
  ): RawServicePumpResult | undefined {
    const state = this.spotMessageFollow.get(spotKey(wire.target.spot));
    if (state === undefined || !sameDirectSpotRoute(state.source, wire.target)) {
      return undefined;
    }
    if (state.expiresAtMs !== undefined && state.expiresAtMs <= Date.now()) {
      this.removeSpotMessageFollow(state);
      return undefined;
    }
    const bytes = ingress.parts.reduce((sum, part) => sum + part.byteLength, 0);
    if (
      state.queuedCount >= MESSAGE_FOLLOW_MESSAGE_LIMIT
      || bytes > MESSAGE_FOLLOW_BYTE_LIMIT - state.queuedBytes
    ) {
      if (wire.kind === 'spotRequest') {
        const result = failure(new ServiceStaleGenerationError('spot', wire.target.spot.spotId));
        this.replyWire(ingress, wire.correlation!, result.terminalResult, result.failureCode);
      }
      return 'infrastructure';
    }
    state.queued.push({
      ingress: retainIngress(ingress),
      wire,
      bytes
    });
    state.queuedCount += 1;
    state.queuedBytes += bytes;
    if (state.target !== undefined) void this.drainSpotMessageFollow(state);
    return 'application';
  }

  private async drainSpotMessageFollow(state: ServiceSpotMessageFollowState): Promise<void> {
    if (state.draining || state.target === undefined) return;
    state.draining = true;
    try {
      while (state.queuedCount > 0 && this.spotMessageFollow.get(state.seal.key) === state) {
        if (state.expiresAtMs === undefined || state.expiresAtMs <= Date.now()) {
          this.failExpiredSpotMessageFollow(state);
          return;
        }
        const current = this.peekSpotMessageFollow(state)!;
        const parts = [
          encodeSpotHeader(
            current.wire.kind,
            current.wire.sourceSpotId,
            state.target,
            current.wire.correlation!
          ),
          current.ingress.parts[1]!
        ];
        if (current.wire.kind === 'spotSend') {
          if (!this.raw.sendService(state.target.targetNodeRid, parts)) {
            this.scheduleSpotMessageFollowRetry(state);
            return;
          }
        } else {
          try {
            const remainingMs = Math.max(
              1,
              Math.min(30_000, state.expiresAtMs - Date.now())
            );
            const reply = await this.raw.requestService(
              state.target.targetNodeRid,
              parts,
              remainingMs
            );
            this.raw.replyService(current.ingress, reply);
          } catch (error) {
            const result = failure(error);
            this.replyWire(
              current.ingress,
              current.wire.correlation!,
              result.terminalResult,
              result.failureCode
            );
          }
        }
        this.notifySpotMessageFollowSource(state, current);
        this.takeSpotMessageFollow(state);
        state.queuedBytes -= current.bytes;
      }
    } finally {
      state.draining = false;
    }
  }

  private scheduleSpotMessageFollowRetry(state: ServiceSpotMessageFollowState): void {
    if (state.retryTimer !== undefined) return;
    state.retryTimer = setTimeout(() => {
      state.retryTimer = undefined;
      void this.drainSpotMessageFollow(state);
    }, 10);
  }

  private notifySpotMessageFollowSource(
    state: ServiceSpotMessageFollowState,
    current: ServiceSpotMessageFollowRecord
  ): void {
    const target = state.target;
    if (target === undefined) return;
    const operationLow = current.ingress.requestSequence !== undefined
      && current.ingress.requestSequence !== 0n
      ? current.ingress.requestSequence
      : this.nextMessageFollowOperation++;
    this.raw.sendService(current.ingress.sourceRoutingId, [encodeMessageFollowHeader({
      source: messageFollowSpotRoute(state.source),
      target: messageFollowSpotRoute(target),
      hopCount: 1,
      queuedMessages: state.queuedCount,
      queuedBytes: state.queuedBytes,
      originalOperation: {
        high: this.nodeGeneration,
        low: operationLow
      },
      originalReplyRouteId: current.ingress.requestSequence ?? 0n
    })]);
  }

  private failExpiredSpotMessageFollow(state: ServiceSpotMessageFollowState): void {
    this.removeSpotMessageFollow(state);
    for (let index = state.queuedHead; index < state.queued.length; index += 1) {
      const current = state.queued[index];
      if (current === undefined) continue;
      if (current.wire.kind !== 'spotRequest') continue;
      const result = failure(
        new ServiceStaleGenerationError('spot', current.wire.target.spot.spotId)
      );
      this.replyWire(
        current.ingress,
        current.wire.correlation!,
        result.terminalResult,
        result.failureCode
      );
    }
  }

  private exactSpotMessageFollow(
    seal: ServiceSpotMessageFollowSeal
  ): ServiceSpotMessageFollowState | undefined {
    const state = this.spotMessageFollow.get(seal.key);
    return state?.seal.serial === seal.serial ? state : undefined;
  }

  private removeSpotMessageFollow(state: ServiceSpotMessageFollowState): void {
    if (this.spotMessageFollow.get(state.seal.key) === state) {
      this.spotMessageFollow.delete(state.seal.key);
    }
    if (state.retryTimer !== undefined) clearTimeout(state.retryTimer);
    state.retryTimer = undefined;
  }

  private peekSpotMessageFollow(
    state: ServiceSpotMessageFollowState
  ): ServiceSpotMessageFollowRecord | undefined {
    while (state.queuedHead < state.queued.length && state.queued[state.queuedHead] === undefined) {
      state.queuedHead += 1;
    }
    return state.queued[state.queuedHead];
  }

  private takeSpotMessageFollow(state: ServiceSpotMessageFollowState): void {
    const current = this.peekSpotMessageFollow(state);
    if (current === undefined) return;
    state.queued[state.queuedHead] = undefined;
    state.queuedHead += 1;
    state.queuedCount -= 1;
    if (state.queuedCount === 0) {
      state.queued.length = 0;
      state.queuedHead = 0;
    } else if (state.queuedHead >= 1024 && state.queuedHead * 2 >= state.queued.length) {
      state.queued.splice(0, state.queuedHead);
      state.queuedHead = 0;
    }
  }

  private actorFence(actor: ServiceActorRef): ServiceActorRouteFence {
    const route = this.tryActorFence(actor);
    if (route === undefined) throw new ServiceStaleGenerationError('actor', actor.actorId);
    return route;
  }

  private tryActorFence(actor: ServiceActorRef): ServiceActorRouteFence | undefined {
    const local = actor.nodeRid === this.nodeRid ? this.registry.actor(actor.actorId) : undefined;
    if (local !== undefined && sameActorRef(local.ref, actor)) {
      return {
        actor,
        targetNodeGeneration: this.nodeGeneration,
        authorityOwnerGeneration: local.authorityOwnerGeneration
      };
    }
    const route = this.actorRoutes.get(actorKey(actor));
    return route !== undefined && sameActorRef(route.actor, actor) ? route : undefined;
  }

  private trySpotFence(
    targetNodeRid: string,
    spot: ServiceSpotRef
  ): ServiceSpotRouteFence | undefined {
    const local = targetNodeRid === this.nodeRid ? this.registry.spot(spot.spotId) : undefined;
    if (local !== undefined && sameSpotRef(local.ref, spot)) {
      return {
        spot,
        targetNodeRid,
        targetNodeGeneration: this.nodeGeneration,
        authorityOwnerGeneration: local.authorityOwnerGeneration
      };
    }
    const route = this.spotRoutes.get(spotKey(spot));
    return route !== undefined
      && route.targetNodeRid === targetNodeRid
      && sameSpotRef(route.spot, spot)
      ? route
      : undefined;
  }

  private tryEntrySpotFence(targetNodeRid: string): ServiceDirectSpotRouteFence | undefined {
    const targetNodeGeneration = targetNodeRid === this.nodeRid
      ? this.nodeGeneration
      : this.raw.topology.peer(targetNodeRid)?.descriptor.lifecycleGeneration;
    if (targetNodeGeneration === undefined) return undefined;
    return {
      spot: {
        spotId: targetNodeRid,
        generation: targetNodeGeneration
      },
      targetNodeRid,
      targetNodeGeneration,
      authorityOwnerGeneration: targetNodeGeneration,
      ownerLeaseGeneration: targetNodeGeneration,
      storeVersion: `entry:${targetNodeGeneration}`
    };
  }

  private acceptSpotAuthority(
    requested: ServiceDirectSpotRouteFence
  ): ServiceDirectSpotRouteFence | undefined {
    const { targetNodeRid, spot, targetNodeGeneration } = requested;
    if (targetNodeRid === this.nodeRid) {
      const local = this.registry.spot(spot.spotId);
      if (
        local?.kind === 'entry'
        && sameSpotRef(local.ref, spot)
        && targetNodeGeneration === this.nodeGeneration
        && requested.authorityOwnerGeneration === local.authorityOwnerGeneration
        && isEntrySpotFence(requested)
      ) {
        return requested;
      }
      const current = this.directSpotRoutes.get(directSpotKey(spot.spotId));
      return local !== undefined
        && local.state === 'ready'
        && targetNodeGeneration === this.nodeGeneration
        && current !== undefined
        && current.targetNodeRid === this.nodeRid
        && current.targetNodeGeneration === this.nodeGeneration
        && current.ownerLeaseGeneration === requested.ownerLeaseGeneration
        && (
          current.spot.generation !== requested.spot.generation
          || (
            current.authorityOwnerGeneration === requested.authorityOwnerGeneration
          )
        )
        ? current
        : undefined;
    }
    const peer = this.raw.topology.peer(targetNodeRid);
    if (peer?.descriptor.lifecycleGeneration !== targetNodeGeneration) {
      return undefined;
    }
    this.rememberSpotRoute(requested);
    return this.directSpotRoutes.get(directSpotKey(spot.spotId));
  }

  private peerGeneration(nodeRid: string): bigint {
    if (nodeRid === this.nodeRid) return this.nodeGeneration;
    const peer = this.raw.topology.peer(nodeRid);
    if (peer === undefined) {
      throw new Error(`Node '${nodeRid}' has no admitted lifecycle generation.`);
    }
    return peer.descriptor.lifecycleGeneration;
  }

  private commitJoinedActor(actor: ServiceActorRef, spot: ServiceSpotRef, membershipEpoch: bigint): void {
    const current = this.registry.actor(actor.actorId);
    const previousActor = current?.ref ?? actor;
    const previousSpot = current?.spot;
    const previousSpotState = previousSpot === undefined
      ? undefined
      : this.registry.spot(previousSpot.spotId);
    const previousMembershipEpoch = current?.membershipEpoch ?? membershipEpoch - 1n;
    if (current === undefined) {
      this.registry.restoreActor(
        { ...actor, nodeRid: this.nodeRid },
        'actor',
        spot,
        membershipEpoch
      );
    } else {
      this.registry.joinActor(current.ref, spot);
    }
    const committed = this.registry.requireActor({
      ...actor,
      nodeRid: this.nodeRid
    });
    const targetSpot = this.registry.spot(spot.spotId);
    if (targetSpot?.kind !== 'entry') {
      this.enqueueActorControl(spot.spotId, {
        kind: 'actorControl',
        lifecycleKind: ActorLifecycleKind.Joined,
        previousActor,
        currentActor: committed.ref,
        previousSpotId: previousSpot?.spotId ?? null,
        currentSpotId: spot.spotId,
        previousSpotGeneration: previousSpot?.generation ?? 0n,
        currentSpotGeneration: spot.generation,
        previousMembershipEpoch,
        currentMembershipEpoch: committed.membershipEpoch,
        resultCode: 0
      });
    }
    if (
      actor.nodeRid === this.nodeRid
      && previousSpot !== undefined
      && previousSpot.spotId !== spot.spotId
      && previousSpotState?.ref.generation === previousSpot.generation
      && (previousSpotState.kind === 'entry' || previousSpotState.kind === 'user')
    ) {
      // A same-node Core join commits the target membership and publishes
      // the source LEFT control separately. Remote joins use the formal
      // transfer source terminal instead; the wire ActorRef still identifies
      // the source owner, so a remote target must not publish a second LEFT.
      this.enqueueActorControl(previousSpot.spotId, {
        kind: 'actorControl',
        lifecycleKind: ActorLifecycleKind.Left,
        previousActor,
        currentActor: previousActor,
        previousSpotId: previousSpot.spotId,
        currentSpotId: spot.spotId,
        previousSpotGeneration: previousSpot.generation,
        currentSpotGeneration: spot.generation,
        previousMembershipEpoch,
        currentMembershipEpoch: committed.membershipEpoch,
        resultCode: 0
      });
    }
  }

  private hasCommittedActorJoin(transferId: string): boolean {
    const expiresAt = this.actorJoinCommittedTransfers.get(transferId);
    if (expiresAt === undefined) return false;
    if (expiresAt <= Date.now()) {
      this.actorJoinCommittedTransfers.delete(transferId);
      return false;
    }
    return true;
  }

  private rememberCommittedActorJoin(transferId: string): void {
    this.actorJoinCommittedTransfers.set(transferId, Date.now() + 30_000);
  }

  private hasSourceLeave(transferId: string): boolean {
    const expiresAt = this.actorJoinSourceLeaves.get(transferId);
    if (expiresAt === undefined) return false;
    if (expiresAt <= Date.now()) {
      this.actorJoinSourceLeaves.delete(transferId);
      return false;
    }
    return true;
  }

  private rememberSourceLeave(transferId: string): void {
    this.actorJoinSourceLeaves.set(transferId, Date.now() + 30_000);
  }

  private requireOpen(): void {
    if (this.closed) throw new Error('Stateful runtime is closed.');
  }
}

function isEntrySpotFence(fence: ServiceDirectSpotRouteFence): boolean {
  return fence.spot.spotId === fence.targetNodeRid
    && fence.spot.generation === fence.targetNodeGeneration
    && fence.authorityOwnerGeneration === fence.targetNodeGeneration
    && fence.ownerLeaseGeneration === fence.targetNodeGeneration
    && fence.storeVersion === `entry:${fence.targetNodeGeneration}`;
}

function userSpotDeadline(deadlineUnixMs: bigint): {
  readonly signal: AbortSignal;
  close(): void;
} {
  const controller = new AbortController();
  const delay = Number(deadlineUnixMs - BigInt(Date.now()));
  const timeout = setTimeout(
    () => controller.abort(new Error('User Spot operation deadline exceeded.')),
    Math.max(0, Math.min(delay, 0x7fff_ffff))
  );
  return {
    signal: controller.signal,
    close: () => clearTimeout(timeout)
  };
}

function operationReplayExpired(deadlineUnixMs: bigint): boolean {
  return BigInt(Date.now()) > deadlineUnixMs
    + BigInt(USER_SPOT_OPERATION_REPLAY_RETENTION_MS);
}

function remainingDeadlineMs(deadlineUnixMs: bigint): number {
  const remainingMs = deadlineUnixMs - BigInt(Date.now());
  if (remainingMs <= 0n) {
    throw createInternalFrameworkException(
      ZLinkFrameworkInternalErrorKind.DeadlineExceeded,
      'Stateful request exceeded its end-to-end deadline.',
      true
    );
  }
  return Number(remainingMs);
}

function userSpotOperationFingerprint(
  record: ServiceUserSpotCreateRecord | ServiceUserSpotCloseRecord | ServiceActorCreateRecord
): string {
  const { correlation: _correlation, ...semantic } = record;
  return JSON.stringify(
    semantic,
    (_key, value: unknown) => typeof value === 'bigint' ? `${value}n` : value
  );
}

function lookupKindData(actor: ServiceActorState): ReceiveKindData {
  return {
    kind: 'actorLookupCompletion',
    location: actorLocation(actor)
  };
}

function actorLocation(actor: ServiceActorState): ActorLocation {
  return {
    actor: actor.ref,
    spotId: actor.spot.spotId,
    spotGeneration: actor.spot.generation,
    membershipEpoch: actor.membershipEpoch
  };
}

function failure(error: unknown): ServiceStatefulResult {
  if (error instanceof ServiceStaleGenerationError) {
    return { terminalResult: RequestResult.NotFound, failureCode: ACTOR_ROUTE_STALE };
  }
  if (error instanceof ZLinkFrameworkException) {
    const kind = internalFrameworkErrorKind(error);
    if (kind === ZLinkFrameworkInternalErrorKind.DeadlineExceeded) {
      return { terminalResult: RequestResult.TimedOut, failureCode: 0 };
    }
    if (kind === ZLinkFrameworkInternalErrorKind.RequestTargetNotFound) {
      return {
        terminalResult: RequestResult.NotFound,
        failureCode: internalFrameworkErrorCode(error) + 1
      };
    }
    if (kind === ZLinkFrameworkInternalErrorKind.SpotGenerationStale
      || kind === ZLinkFrameworkInternalErrorKind.SpotMoving) {
      return {
        terminalResult: RequestResult.Conflict,
        failureCode: internalFrameworkErrorCode(error) + 1
      };
    }
    return {
      terminalResult: RequestResult.InternalError,
      failureCode: internalFrameworkErrorCode(error) + 1
    };
  }
  return { terminalResult: RequestResult.InternalError, failureCode: 17 };
}

function actorKey(actor: ServiceActorRef): string {
  return `${actor.actorId}\0${actor.generation}`;
}

function spotKey(spot: ServiceSpotRef): string {
  return `${spot.spotId}\0${spot.generation}`;
}

function directSpotKey(spotId: string): string {
  return spotId;
}

function sameActorRef(left: ServiceActorRef, right: ServiceActorRef): boolean {
  return left.nodeRid === right.nodeRid
    && left.actorId === right.actorId
    && left.generation === right.generation;
}

function sameSpotRef(left: ServiceSpotRef, right: ServiceSpotRef): boolean {
  return left.spotId === right.spotId && left.generation === right.generation;
}

function freezeSpotRoute(route: ServiceSpotRouteFence): ServiceSpotRouteFence {
  return Object.freeze({
    spot: Object.freeze({ ...route.spot }),
    targetNodeRid: route.targetNodeRid,
    targetNodeGeneration: route.targetNodeGeneration,
    authorityOwnerGeneration: route.authorityOwnerGeneration
  });
}

function freezeDirectSpotRoute(
  route: ServiceDirectSpotRouteFence
): ServiceDirectSpotRouteFence {
  return Object.freeze({
    ...freezeSpotRoute(route),
    ownerLeaseGeneration: route.ownerLeaseGeneration,
    storeVersion: route.storeVersion
  });
}

function retainIngress(record: RawServiceIngressRecord): RawServiceIngressRecord {
  // Raw binding ingress already owns stable Buffer values. Relocation follow
  // queues retain the same record until the route decision completes.
  return record;
}

function statefulCorrelation(record: ServiceStatefulWireRecord): bigint | undefined {
  if ('correlation' in record) return record.correlation;
  return record.kind === 'instanceSpot' && record.operationKind === 'request'
    ? record.replyRouteId
    : undefined;
}

function matchesExpectedInstanceRoute(
  current: ServiceInstanceRouteFence | undefined,
  expected: ServiceInstanceRouteFence | null | undefined
): boolean {
  if (expected === undefined) return true;
  return expected === null
    ? current === undefined
    : current !== undefined && sameInstanceRoute(current, expected);
}

function sameInstanceRoute(left: ServiceInstanceRouteFence, right: ServiceInstanceRouteFence): boolean {
  return left.targetNodeRid === right.targetNodeRid
    && left.targetNodeGeneration === right.targetNodeGeneration
    && left.targetSpotId === right.targetSpotId
    && left.objectGeneration === right.objectGeneration
    && left.ownerId === right.ownerId
    && left.authorityOwnerGeneration === right.authorityOwnerGeneration
    && left.leaseGeneration === right.leaseGeneration
    && left.storeVersion === right.storeVersion;
}

function sameInstanceOwnerIdentity(
  left: ServiceInstanceRouteFence,
  right: ServiceInstanceRouteFence
): boolean {
  return left.targetNodeRid === right.targetNodeRid
    && left.targetNodeGeneration === right.targetNodeGeneration
    && left.targetSpotId === right.targetSpotId
    && left.objectGeneration === right.objectGeneration
    && left.ownerId === right.ownerId
    && left.authorityOwnerGeneration === right.authorityOwnerGeneration
    && left.leaseGeneration === right.leaseGeneration;
}

function sameInstanceApplicationOwner(
  left: ServiceInstanceRouteFence,
  right: ServiceInstanceRouteFence
): boolean {
  return left.targetNodeRid === right.targetNodeRid
    && left.targetNodeGeneration === right.targetNodeGeneration
    && left.targetSpotId === right.targetSpotId
    && left.ownerId === right.ownerId
    && left.leaseGeneration === right.leaseGeneration;
}

function matchesExpectedDirectSpotRoute(
  current: ServiceDirectSpotRouteFence | undefined,
  expected: ServiceDirectSpotRouteFence | null
): boolean {
  return expected === null
    ? current === undefined
    : current !== undefined && sameDirectSpotRoute(current, expected);
}

function instanceRouteAsDirectRoute(route: ServiceInstanceRouteFence): ServiceDirectSpotRouteFence {
  return {
    spot: {
      spotId: route.targetSpotId,
      generation: route.objectGeneration
    },
    targetNodeRid: route.targetNodeRid,
    targetNodeGeneration: route.targetNodeGeneration,
    authorityOwnerGeneration: route.authorityOwnerGeneration,
    ownerLeaseGeneration: route.leaseGeneration,
    storeVersion: route.storeVersion
  };
}

function sameDirectSpotRoute(
  left: ServiceDirectSpotRouteFence,
  right: ServiceDirectSpotRouteFence
): boolean {
  return sameDirectSpotRouteIdentity(left, right)
    && left.storeVersion === right.storeVersion;
}

function messageFollowSpotRoute(
  route: ServiceDirectSpotRouteFence
): Extract<ServiceMessageFollowRoute, { readonly kind: 'spot' }> {
  return {
    kind: 'spot',
    spot: route.spot,
    targetNodeRid: route.targetNodeRid,
    targetNodeGeneration: route.targetNodeGeneration,
    authorityOwnerGeneration: route.authorityOwnerGeneration,
    ownerLeaseGeneration: route.ownerLeaseGeneration
  };
}

function sameMessageFollowSpotSource(
  current: ServiceDirectSpotRouteFence,
  source: Extract<ServiceMessageFollowRoute, { readonly kind: 'spot' }>
): boolean {
  return current.spot.spotId === source.spot.spotId
    && current.spot.generation === source.spot.generation
    && current.targetNodeRid === source.targetNodeRid
    && current.targetNodeGeneration === source.targetNodeGeneration
    && current.authorityOwnerGeneration === source.authorityOwnerGeneration
    && current.ownerLeaseGeneration === source.ownerLeaseGeneration;
}

function sameDirectSpotRouteIdentity(
  left: ServiceDirectSpotRouteFence,
  right: ServiceDirectSpotRouteFence
): boolean {
  return sameDirectSpotOwnerIdentity(left, right)
    && left.spot.generation === right.spot.generation;
}

function sameDirectSpotOwnerIdentity(
  left: ServiceDirectSpotRouteFence,
  right: ServiceDirectSpotRouteFence
): boolean {
  return left.spot.spotId === right.spot.spotId
    && left.targetNodeRid === right.targetNodeRid
    && left.targetNodeGeneration === right.targetNodeGeneration
    && left.authorityOwnerGeneration === right.authorityOwnerGeneration
    && left.ownerLeaseGeneration === right.ownerLeaseGeneration;
}

function routeMatchesLocal(
  route: ServiceInstanceRouteFence,
  spot: ServiceSpotState,
  nodeRid: string,
  nodeGeneration: bigint
): boolean {
  return route.targetNodeRid === nodeRid
    && route.targetNodeGeneration === nodeGeneration
    && route.targetSpotId === spot.ref.spotId
    && route.objectGeneration === spot.ref.generation
    && route.authorityOwnerGeneration === spot.authorityOwnerGeneration;
}

function instanceOperationKey(
  record: Extract<ServiceStatefulWireRecord, { readonly kind: 'instanceSpot' }>
): string {
  return `${record.sourceNodeRid}\0${record.sourceNodeGeneration}\0`
    + `${record.operation.high}\0${record.operation.low}`;
}

function instanceActivationKey(
  record: Extract<ServiceStatefulWireRecord, { readonly kind: 'instanceSpot'; readonly activation: 'missing' }>
): string {
  return `${record.target.targetNodeRid}\0${record.target.targetNodeGeneration}\0`
    + `${record.target.targetSpotId}\0${record.target.stableType}`;
}

function subscriptionKey(channelName: string, topicFilter: string): string {
  if (channelName.length === 0 || topicFilter.length === 0) {
    throw new TypeError('Channel name and topic filter must be non-empty.');
  }
  return `${channelName}\0${topicFilter}`;
}

function topicMatches(filter: string, topic: string): boolean {
  if (filter === '*' || filter === '#') return true;
  if (filter.endsWith('*')) return topic.startsWith(filter.slice(0, -1));
  return filter === topic;
}

function emptyPayload(): ServiceApplicationPayload {
  return {
    packetName: 'ZLinkFrameworkEmpty',
    contentType: 'application/octet-stream',
    payload: Buffer.alloc(0)
  };
}

function actorJoinPhaseFromApplicationFrame(
  frame: Uint8Array | undefined
): 'admission' | 'commit' | 'abort' | undefined {
  return actorJoinMetadataFromApplicationFrame(frame)?.phase;
}

function actorJoinTransferIdFromApplicationFrame(
  frame: Uint8Array | undefined
): string | undefined {
  return actorJoinMetadataFromApplicationFrame(frame)?.transferId;
}

function actorJoinMetadataFromApplicationFrame(
  frame: Uint8Array | undefined
): {
  readonly phase: 'admission' | 'commit' | 'abort' | undefined;
  readonly transferId: string | undefined;
} | undefined {
  if (frame === undefined) return undefined;
  try {
    return actorJoinMetadataFromMultipartPayload(decodeApplicationPayload(frame).payload);
  } catch {
    return undefined;
  }
}

function actorJoinMetadataFromMultipartPayload(
  payload: Uint8Array | undefined
): {
  readonly phase: 'admission' | 'commit' | 'abort' | undefined;
  readonly transferId: string | undefined;
} | undefined {
  if (payload === undefined || payload.byteLength < 8) return undefined;
  const bytes = Buffer.from(payload.buffer, payload.byteOffset, payload.byteLength);
  const partCount = bytes.readUInt32BE(0);
  if (partCount < 1 || bytes.length < 8) return undefined;
  const partLength = bytes.readUInt32BE(4);
  if (partLength > bytes.length - 8) return undefined;
  let decoded: unknown;
  try {
    decoded = JSON.parse(bytes.subarray(8, 8 + partLength).toString('utf8'));
  } catch {
    return undefined;
  }
  if (typeof decoded !== 'object' || decoded === null) return undefined;
  const phase = (decoded as { readonly phase?: unknown }).phase;
  const transferId = (decoded as { readonly transferId?: unknown }).transferId;
  return {
    phase: phase === 'admission' || phase === 'commit' || phase === 'abort'
      ? phase
      : undefined,
    transferId: typeof transferId === 'string' ? transferId : undefined
  };
}

function rawPeerRouteReady(
  raw: RawServiceMeshRuntime,
  targetNodeRid: string
): boolean | undefined {
  const candidate = raw as unknown as {
    isPeerRouteReady?: (nodeRoutingId: string) => boolean;
  };
  if (typeof candidate.isPeerRouteReady !== 'function') return undefined;
  return candidate.isPeerRouteReady.call(raw, targetNodeRid);
}

function instanceOperationParts(
  headerAndPayload: readonly Buffer[],
  metadataFrame?: Uint8Array
): readonly Buffer[] {
  if (headerAndPayload.length !== 2) {
    throw new TypeError('Instance operation requires one header and one payload frame.');
  }
  if (metadataFrame === undefined) return headerAndPayload;
  return [
    headerAndPayload[0]!,
    validateServiceMetadataFrame(metadataFrame),
    headerAndPayload[1]!
  ];
}

function statefulDebugTrace(event: string, fields: readonly string[]): void {
  const mode = process.env.ZLINK_NODE_FILE_TRACE_DEBUG;
  if (mode !== '1' && mode !== 'errors') return;
  if (mode === 'errors' && !event.endsWith('-error')) return;
  try {
    appendFileSync(
      process.env.ZLINK_NODE_FILE_TRACE_PATH ?? '/tmp/zlink-node-stateful-debug.log',
      `${new Date().toISOString()} pid=${process.pid} ${event} ${fields.join(' ')}\n`
    );
  } catch {
    // Diagnostic tracing must never affect runtime behavior.
  }
}

export function statefulMailboxData(record: ServiceMailboxRecord): ServiceStatefulMailboxData | undefined {
  return record.stateful as ServiceStatefulMailboxData | undefined;
}
