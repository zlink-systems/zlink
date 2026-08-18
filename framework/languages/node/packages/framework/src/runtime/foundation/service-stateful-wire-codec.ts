import type {
  ServiceActorRef,
  ServiceSessionBinding,
  ServiceSpotRef
} from './service-stateful-registry';
import { operationRequiresReply } from './service-runtime-contracts';
import { ServiceWireProtocolError } from './service-wire-m6a-codec';
import { isCanonicalWireReplyTerminal } from '../framework-errors-internal';
import { routingIdsEqual } from '../routing-id';
import {
  SERVICE_WIRE_MAGIC,
  SERVICE_WIRE_MAJOR,
  ServiceWireCommand,
  ServiceWireFlag
} from './service-wire-constants.generated';
import {
  decodeCanonicalServiceWireText,
  decodeServiceWireRoutingId,
  encodeCanonicalServiceWireText,
  encodeServiceWireRoutingId
} from './service-wire-binary-primitives';

const PREFIX_SIZE = 5;
const MAGIC_0 = SERVICE_WIRE_MAGIC[0];
const MAGIC_1 = SERVICE_WIRE_MAGIC[1];
const MAJOR = SERVICE_WIRE_MAJOR;

export const M6bServiceWireCommand = ServiceWireCommand;

export interface ServiceWireOperationId {
  readonly high: bigint;
  readonly low: bigint;
}

export interface ServiceWireRelocationCoordinatorFence {
  readonly ownerId: string;
  readonly leaseGeneration: bigint;
  readonly nodeRid: string;
  readonly nodeGeneration: bigint;
  readonly expectedAuthorityStoreVersion: string;
}

export interface ServiceWireRequestSourceFence {
  readonly ownerId: string;
  readonly leaseGeneration: bigint;
  readonly nodeRid: string;
  readonly nodeGeneration: bigint;
}

export interface ServiceMaintenanceReplyRelay {
  readonly relocation: ServiceWireOperationId;
  readonly targetAttemptGeneration: bigint;
  readonly coordinator: ServiceWireRelocationCoordinatorFence;
  readonly operation: ServiceWireOperationId;
  readonly replyRouteId: bigint;
  readonly participantId: bigint;
  readonly sequence: bigint;
  readonly terminalResult: number;
  readonly failureCode: number;
  readonly payload?: {
    readonly packetName: string;
    readonly contentType: string;
    readonly bytes: Uint8Array;
  };
}

export interface ServiceMaintenanceReplyRelayAck {
  readonly relocation: ServiceWireOperationId;
  readonly coordinator: ServiceWireRelocationCoordinatorFence;
  readonly operation: ServiceWireOperationId;
  readonly replyRouteId: bigint;
  readonly requestSource: ServiceWireRequestSourceFence;
  readonly status: 'terminalReceived' | 'alreadyTerminal';
}

export type ServiceWireRelocationRole = 'source' | 'target' | 'coordinator';

export interface ServiceWireRelocationTarget {
  readonly nodeRid: string;
  readonly nodeGeneration: bigint;
  readonly ownerId: string;
  readonly ownerLeaseGeneration: bigint;
}

export type ServiceWireRelocationObject =
  | {
      readonly kind: 'actor';
      readonly actorId: string;
      readonly objectGeneration: bigint;
      readonly expectedAuthorityOwnerGeneration: bigint;
    }
  | {
      readonly kind: 'userSpot';
      readonly spotId: string;
      readonly objectGeneration: bigint;
      readonly expectedAuthorityOwnerGeneration: bigint;
    }
  | {
      readonly kind: 'instanceSpot';
      readonly stableType: string;
      readonly spotId: string;
      readonly objectGeneration: bigint;
    };

/** Shared CRC-32C wire bounds for the direct relocation payload transfer. */
export const RELOCATION_PAYLOAD_TOTAL_LENGTH_MAX = 274_877_906_944n;
export const RELOCATION_PAYLOAD_CHUNK_COUNT_MAX = 4096;
export const RELOCATION_STATE_CHUNK_DATA_MAX_BYTES = 67_108_864;

interface ServiceWireRelocationBase {
  readonly relocation: ServiceWireOperationId;
  readonly targetAttemptGeneration: bigint;
  readonly coordinator: ServiceWireRelocationCoordinatorFence;
}

export interface ServiceMaintenanceRelocationPrepare extends ServiceWireRelocationBase {
  readonly kind: 'prepare';
  readonly target: ServiceWireRelocationTarget;
  readonly initiatorRole: ServiceWireRelocationRole;
  readonly object: ServiceWireRelocationObject;
  readonly sourceNodeRid: string;
  readonly sourceNodeGeneration: bigint;
  /** Total encoded byte length of the directly transferred relocation payload. */
  readonly payloadTotalLength: bigint;
  /** Number of relocationState chunks that carry the payload. */
  readonly payloadChunkCount: number;
  /** CRC-32C (Castagnoli) over the fully assembled payload bytes. */
  readonly payloadChecksumCrc32c: number;
  /** CRC-32C over the pre-seal base snapshot already sent as base-stage chunks. 0 = no base. */
  readonly baseChecksumCrc32c: number;
  readonly applicationVersion: bigint;
}

export interface ServiceMaintenanceRelocationReady extends ServiceWireRelocationBase {
  readonly kind: 'ready';
  readonly target: ServiceWireRelocationTarget;
  readonly object: ServiceWireRelocationObject;
  readonly senderRole: ServiceWireRelocationRole;
}

/** Explicit target-side pre-cutover relocation failure (command 53). */
export interface ServiceMaintenanceRelocationFailed extends ServiceWireRelocationBase {
  readonly kind: 'failed';
  readonly target: ServiceWireRelocationTarget;
  readonly object: ServiceWireRelocationObject;
  readonly senderRole: ServiceWireRelocationRole;
  readonly failureCode: number;
}

export interface ServiceWireFrozenRecord {
  readonly recordKind: number;
  readonly sourceKind: number;
  readonly source: ServiceWireRequestSourceFence;
  readonly sourceSpotId?: string;
  readonly sourceActor?: { readonly actorId: string; readonly generation: bigint };
  readonly sourceSessionRid?: string;
  readonly sourceBindingGeneration?: bigint;
  readonly sourceSessionSequence?: bigint;
  readonly hasMetadata: boolean;
  readonly operationId: ServiceWireOperationId;
  readonly operationKind: number;
  readonly replyRouteId?: bigint;
  /** Decoded routing/application body used by the target temporary queue owner. */
  readonly target?:
    | {
        readonly kind: 'spot';
        readonly spotId: string;
        readonly generation: bigint;
        readonly targetNodeRid: string;
        readonly targetNodeGeneration: bigint;
        readonly authorityOwnerGeneration: bigint;
        readonly ownerLeaseGeneration: bigint;
      }
    | {
        readonly kind: 'actor';
        readonly actorId: string;
        readonly generation: bigint;
        readonly targetNodeRid: string;
        readonly targetNodeGeneration: bigint;
        readonly authorityOwnerGeneration: bigint;
        readonly ownerLeaseGeneration: bigint;
      };
  readonly applicationPayload?: NonNullable<ServiceMaintenanceReplyRelay['payload']>;
  readonly canonicalBytes: Buffer;
}

export interface ServiceMaintenanceRelocationData extends ServiceWireRelocationBase {
  readonly kind: 'data';
  readonly senderRole: ServiceWireRelocationRole;
  readonly object: ServiceWireRelocationObject;
  readonly frozenRecord: ServiceWireFrozenRecord;
}

export interface ServiceMaintenanceRelocationCutover extends ServiceWireRelocationBase {
  readonly kind: 'cutover';
  readonly senderRole: ServiceWireRelocationRole;
  readonly object: ServiceWireRelocationObject;
  /** Number of boundary relay records sent before this cutover. */
  readonly boundaryRecordCount: bigint;
  /** CRC-32C over the concatenated canonical bytes of those relay records. */
  readonly boundaryChecksumCrc32c: number;
}

export type ServiceWireRelocationPayloadStage = 'base' | 'final';

/** One relocation payload chunk (command 52), sent one-way on the ordered connection. */
export interface ServiceMaintenanceRelocationState extends ServiceWireRelocationBase {
  readonly kind: 'state';
  readonly senderRole: ServiceWireRelocationRole;
  readonly object: ServiceWireRelocationObject;
  /** Independent ordinal space per stage: base (pre-seal snapshot) or final (delta/full payload). */
  readonly payloadStage: ServiceWireRelocationPayloadStage;
  readonly chunkOrdinal: number;
  readonly chunkData: Uint8Array;
}

export type ServiceMaintenanceRelocationControl =
  | ServiceMaintenanceRelocationPrepare
  | ServiceMaintenanceRelocationReady
  | ServiceMaintenanceRelocationFailed
  | ServiceMaintenanceRelocationData
  | ServiceMaintenanceRelocationCutover
  | ServiceMaintenanceRelocationState;

export const M6bServiceWireFlag = ServiceWireFlag;

export interface ServiceSpotRouteFence {
  readonly spot: ServiceSpotRef;
  readonly targetNodeRid: string;
  readonly targetNodeGeneration: bigint;
  readonly authorityOwnerGeneration: bigint;
}

export interface ServiceDirectSpotRouteFence extends ServiceSpotRouteFence {
  readonly ownerLeaseGeneration: bigint;
  readonly storeVersion: string;
}

export interface ServiceActorRouteFence {
  readonly actor: ServiceActorRef;
  readonly targetNodeGeneration: bigint;
  readonly authorityOwnerGeneration: bigint;
  readonly ownerLeaseGeneration: bigint;
}

/**
 * The authority fence carried by a bound-session replacement notice.  This
 * command is deliberately separate from the legacy Actor message fence: the
 * receiver validates the transport source against this authority target and
 * must not resolve a local Actor object for the retired session.
 */
export interface ServiceBoundSessionActorAuthority {
  readonly actor: ServiceActorRef;
  readonly targetNodeGeneration: bigint;
  readonly authorityOwnerGeneration: bigint;
  readonly ownerLeaseGeneration: bigint;
}

export interface ServiceSessionRelocationOwnerFence {
  readonly sessionOwnerNodeRid: string;
  readonly sessionOwnerNodeGeneration: bigint;
  readonly sessionOwnerId: string;
  readonly sessionOwnerLeaseGeneration: bigint;
  readonly sessionRid: string;
  readonly bindingGeneration: bigint;
}

export interface ServiceSessionRelocationSeal {
  readonly relocation: ServiceWireOperationId;
  readonly coordinator: ServiceWireRelocationCoordinatorFence;
  readonly senderRole: 'source' | 'coordinator';
  readonly actor: ServiceBoundSessionActorAuthority;
  readonly session: ServiceSessionRelocationOwnerFence;
}

export interface ServiceSessionRelocationSealed {
  readonly relocation: ServiceWireOperationId;
  readonly coordinator: ServiceWireRelocationCoordinatorFence;
  readonly actor: ServiceBoundSessionActorAuthority;
  readonly session: ServiceSessionRelocationOwnerFence;
}

export type ServiceSessionRelocationRoute =
  | {
      readonly relocation: ServiceWireOperationId;
      readonly coordinator: ServiceWireRelocationCoordinatorFence;
      readonly senderRole: 'target' | 'coordinator';
      readonly actor: ServiceActorRef;
      readonly session: ServiceSessionRelocationOwnerFence;
      readonly route: {
        readonly action: 'commit';
        readonly previousAuthorityOwnerGeneration: bigint;
        readonly targetAuthorityOwnerGeneration: bigint;
        readonly targetNodeRid: string;
        readonly targetNodeGeneration: bigint;
      };
    }
  | {
      readonly relocation: ServiceWireOperationId;
      readonly coordinator: ServiceWireRelocationCoordinatorFence;
      readonly senderRole: 'source' | 'coordinator';
      readonly actor: ServiceActorRef;
      readonly session: ServiceSessionRelocationOwnerFence;
      readonly route: {
        readonly action: 'abort';
        readonly currentAuthorityOwnerGeneration: bigint;
      };
    };

export interface ServiceRetiredBoundSessionRouteFence {
  readonly sessionOwnerNodeRid: string;
  readonly sessionOwnerNodeGeneration: bigint;
  readonly sessionOwnerId: string;
  readonly sessionOwnerLeaseGeneration: bigint;
  readonly sessionRid: string;
  readonly retiredBindingGeneration: bigint;
}

export interface ServiceBoundSessionSource {
  readonly sessionRid: string;
  readonly bindingGeneration: bigint;
  readonly sequence: bigint;
}

export interface ServiceInstanceRouteFence {
  readonly targetNodeRid: string;
  readonly targetNodeGeneration: bigint;
  readonly targetSpotId: string;
  readonly objectGeneration: bigint;
  readonly ownerId: string;
  readonly authorityOwnerGeneration: bigint;
  readonly leaseGeneration: bigint;
  readonly storeVersion: string;
}

export interface ServiceInstanceActivationTarget {
  readonly targetNodeRid: string;
  readonly targetNodeGeneration: bigint;
  readonly targetSpotId: string;
  readonly stableType: string;
  readonly descriptorVersion: string;
}

export interface ServiceUserSpotReservationFence {
  readonly reservationId: string;
  readonly expectedStoreVersion: string;
  readonly objectGeneration: bigint;
  readonly authorityOwnerGeneration: bigint;
  readonly targetNodeRid: string;
  readonly targetNodeGeneration: bigint;
  readonly targetOwnerId: string;
  readonly targetOwnerLeaseGeneration: bigint;
  readonly pendingCapacityDelta: number;
}

export interface ServiceUserSpotCreateRecord {
  readonly kind: 'userSpotCreate';
  readonly correlation: bigint;
  readonly operation: { readonly high: bigint; readonly low: bigint };
  readonly sourceNodeRid: string;
  readonly sourceNodeGeneration: bigint;
  readonly spotId: string;
  readonly stableType: string;
  readonly reservation: ServiceUserSpotReservationFence;
  readonly deadlineUnixMs: bigint;
}

export interface ServiceActorCreateRecord {
  readonly kind: 'actorCreate';
  readonly correlation: bigint;
  readonly operation: { readonly high: bigint; readonly low: bigint };
  readonly sourceNodeRid: string;
  readonly sourceNodeGeneration: bigint;
  readonly actorId: string;
  readonly stableType: string;
  readonly reservation: ServiceUserSpotReservationFence;
  readonly deadlineUnixMs: bigint;
}

export interface ServiceUserSpotCloseRecord {
  readonly kind: 'userSpotClose';
  readonly correlation: bigint;
  readonly operation: { readonly high: bigint; readonly low: bigint };
  readonly sourceNodeRid: string;
  readonly sourceNodeGeneration: bigint;
  readonly target: {
    readonly spotId: string;
    readonly objectGeneration: bigint;
    readonly targetNodeRid: string;
    readonly targetNodeGeneration: bigint;
    readonly authorityOwnerGeneration: bigint;
    readonly expectedStoreVersion: string;
  };
  readonly deadlineUnixMs: bigint;
}

export type ServiceMessageFollowRoute =
  | {
      readonly kind: 'actor';
      readonly actor: ServiceActorRef;
      readonly targetNodeRid: string;
      readonly targetNodeGeneration: bigint;
      readonly authorityOwnerGeneration: bigint;
      readonly ownerLeaseGeneration: bigint;
    }
  | {
      readonly kind: 'spot';
      readonly spot: ServiceSpotRef;
      readonly targetNodeRid: string;
      readonly targetNodeGeneration: bigint;
      readonly authorityOwnerGeneration: bigint;
      readonly ownerLeaseGeneration: bigint;
    };

export interface ServiceMessageFollowRecord {
  readonly kind: 'messageFollow';
  readonly source: ServiceMessageFollowRoute;
  readonly target: ServiceMessageFollowRoute;
  readonly hopCount: number;
  readonly queuedMessages: number;
  readonly queuedBytes: number;
  readonly originalOperation: ServiceWireOperationId;
  readonly originalReplyRouteId: bigint;
}

export type ServiceBoundSessionTransition =
  | { readonly state: 'active'; readonly generation: bigint }
  | { readonly state: 'tombstone'; readonly retiredGeneration: bigint };

export type ServiceStatefulWireRecord =
  | {
      readonly kind: 'spotSend' | 'spotRequest';
      readonly correlation?: bigint;
      readonly sourceSpotId: string;
      readonly target: ServiceDirectSpotRouteFence;
    }
  | {
      readonly kind: 'logicalMulticast';
      readonly channelName: string;
      readonly topic: string;
      readonly sourceSpotId: string;
    }
  | {
      readonly kind: 'actorSend' | 'actorRequest';
      readonly correlation?: bigint;
      readonly sourceActor?: ServiceActorRef;
      readonly target: ServiceActorRouteFence;
      readonly boundSession?: ServiceBoundSessionSource;
    }
  | {
      readonly kind: 'actorLookup';
      readonly correlation: bigint;
      readonly actorId: string;
    }
  | {
      readonly kind: 'actorDestroy';
      readonly correlation: bigint;
      readonly actor: ServiceActorRouteFence;
    }
  | {
      readonly kind: 'actorJoin';
      readonly correlation: bigint;
      readonly actor: ServiceActorRouteFence;
      readonly entry: boolean;
      readonly target: ServiceSpotRouteFence;
    }
  | {
      readonly kind: 'boundSessionSend';
      readonly actor: ServiceActorRouteFence;
      readonly expectedBindingGeneration: bigint;
    }
  | {
      readonly kind: 'boundSessionBind';
      readonly correlation: bigint;
      readonly actor: ServiceActorRouteFence;
      readonly sessionRid: string;
      readonly binding: ServiceBoundSessionTransition;
    }
  | {
      readonly kind: 'boundSessionReplaced';
      readonly actorAuthority: ServiceBoundSessionActorAuthority;
      readonly retiredSession: ServiceRetiredBoundSessionRouteFence;
    }
  | {
      readonly kind: 'instanceSpot';
      readonly activation: 'ready';
      readonly route: ServiceInstanceRouteFence;
      readonly sourceNodeGeneration: bigint;
      readonly sourceNodeRid: string;
      readonly sourceSpotId?: string;
      readonly operationKind: 'send' | 'request';
      readonly operation: { readonly high: bigint; readonly low: bigint };
      readonly replyRouteId?: bigint;
    }
  | {
      readonly kind: 'instanceSpot';
      readonly activation: 'missing';
      readonly target: ServiceInstanceActivationTarget;
      readonly sourceNodeGeneration: bigint;
      readonly sourceNodeRid: string;
      readonly sourceSpotId?: string;
      readonly operationKind: 'send' | 'request';
      readonly operation: { readonly high: bigint; readonly low: bigint };
      readonly deadlineUnixMs: bigint;
      readonly replyRouteId?: bigint;
    }
  | ServiceUserSpotCreateRecord
  | ServiceUserSpotCloseRecord
  | ServiceActorCreateRecord
  | ServiceMessageFollowRecord;

export type ServiceStatefulReplyTail =
  | {
      readonly kind: 'actorLookup';
      readonly actor: ServiceActorRef;
      readonly spot: ServiceSpotRef;
      readonly membershipEpoch: bigint;
      readonly authorityOwnerGeneration: bigint;
    }
  | {
      readonly kind: 'actorJoin';
      readonly joinResult: 0 | 1;
      readonly spot?: ServiceSpotRef;
      readonly membershipEpoch?: bigint;
      /** Target's advertised valid receive chunk cap. 0 = not advertised (accepted only). */
      readonly receiveChunkLimitBytes?: number;
    }
  | {
      readonly kind: 'streamBind';
      readonly bindingGeneration: bigint;
      readonly authorityOwnerGeneration: bigint;
    }
  | {
      readonly kind: 'userSpotCreate';
      readonly createResult: 'existing' | 'created' | 'rejected';
      readonly spotId: string;
      readonly objectGeneration: bigint;
    }
  | {
      readonly kind: 'userSpotClose';
      readonly closed: boolean;
    }
  | {
      readonly kind: 'actorCreate';
      readonly createResult: 'existing' | 'created' | 'rejected';
      readonly actor?: ServiceActorRef & { readonly nodeRid: string };
    };

export interface ServiceStatefulReply {
  readonly correlation: bigint;
  readonly terminalResult: number;
  readonly failureCode: number;
  readonly tail?: ServiceStatefulReplyTail;
}

export function encodeSpotHeader(
  kind: 'spotSend' | 'spotRequest',
  sourceSpotId: string,
  target: ServiceDirectSpotRouteFence,
  correlation?: bigint
): Buffer {
  return concat(
    prefix(kind === 'spotSend'
      ? M6bServiceWireCommand.spotSend
      : M6bServiceWireCommand.spotRequest),
    ...(kind === 'spotRequest' ? [u64(requirePositive(correlation, 'correlation'))] : []),
    rid(sourceSpotId, 'sourceSpotId'),
    directSpotFence(target)
  );
}

export function encodeActorHeader(
  kind: 'actorSend' | 'actorRequest',
  target: ServiceActorRouteFence,
  correlation?: bigint,
  sourceActor?: ServiceActorRef,
  boundSession?: ServiceBoundSessionSource
): Buffer {
  const flags = boundSession === undefined
    ? 0
    : M6bServiceWireFlag.boundSession | M6bServiceWireFlag.sourceSpotId;
  return concat(
    prefix(
      kind === 'actorSend'
        ? M6bServiceWireCommand.actorSend
        : M6bServiceWireCommand.actorRequest,
      flags
    ),
    ...(kind === 'actorRequest' ? [u64(requirePositive(correlation, 'correlation'))] : []),
    optionalActor(sourceActor),
    actorFence(target),
    ...(boundSession === undefined
      ? []
      : [
          rid(boundSession.sessionRid, 'sourceSessionRid'),
          u64(boundSession.bindingGeneration),
          u64(boundSession.sequence)
        ])
  );
}

export function encodeLogicalMulticastHeader(
  channelName: string,
  topic: string,
  sourceSpotId: string
): Buffer {
  return concat(
    prefix(M6bServiceWireCommand.logicalMulticast),
    text8(channelName, 'channelName'),
    text8(topic, 'topic'),
    rid(sourceSpotId, 'sourceSpotId')
  );
}

export function encodeActorLookupHeader(correlation: bigint, actorId: string): Buffer {
  return concat(
    prefix(M6bServiceWireCommand.actorLookup),
    u64(correlation),
    text8(actorId, 'actorId')
  );
}

export function encodeActorDestroyHeader(
  correlation: bigint,
  actor: ServiceActorRouteFence
): Buffer {
  return concat(prefix(M6bServiceWireCommand.actorDestroy), u64(correlation), actorFence(actor));
}

export function encodeActorJoinHeader(
  correlation: bigint,
  actor: ServiceActorRouteFence,
  entry: boolean,
  target: ServiceSpotRouteFence
): Buffer {
  return concat(
    prefix(M6bServiceWireCommand.actorJoin),
    u64(correlation),
    actorFence(actor),
    Buffer.of(entry ? 1 : 0),
    spotFence(target)
  );
}

export function encodeBoundSessionSendHeader(
  actor: ServiceActorRouteFence,
  expectedBindingGeneration: bigint
): Buffer {
  return concat(
    prefix(M6bServiceWireCommand.boundSessionSend),
    actorFence(actor),
    u64(expectedBindingGeneration)
  );
}

export function encodeBoundSessionBindHeader(
  correlation: bigint,
  actor: ServiceActorRouteFence,
  sessionRid: string,
  binding: ServiceBoundSessionTransition
): Buffer {
  const body = binding.state === 'active'
    ? u64(binding.generation)
    : u64(binding.retiredGeneration);
  return concat(
    prefix(M6bServiceWireCommand.boundSessionBind),
    u64(correlation),
    actorFence(actor),
    rid(sessionRid, 'sessionRid'),
    Buffer.of(binding.state === 'active' ? 1 : 2),
    u16(body.byteLength),
    body
  );
}

export function encodeBoundSessionReplacedHeader(
  actorAuthority: ServiceBoundSessionActorAuthority,
  retiredSession: ServiceRetiredBoundSessionRouteFence
): Buffer {
  return concat(
    prefix(M6bServiceWireCommand.boundSessionReplaced),
    actorAuthorityFence(actorAuthority),
    rid(retiredSession.sessionOwnerNodeRid, 'sessionOwnerNodeRid'),
    u64(retiredSession.sessionOwnerNodeGeneration),
    text8(retiredSession.sessionOwnerId, 'sessionOwnerId'),
    u64(retiredSession.sessionOwnerLeaseGeneration),
    rid(retiredSession.sessionRid, 'sessionRid'),
    u64(retiredSession.retiredBindingGeneration)
  );
}

/** Encodes canonical service-wire command 42. */
export function encodeSessionRelocationSeal(value: ServiceSessionRelocationSeal): Buffer {
  if (value.senderRole !== 'source' && value.senderRole !== 'coordinator') {
    fail('Session relocation seal sender role is invalid.');
  }
  return concat(
    prefix(M6bServiceWireCommand.sessionRelocationSeal),
    wireId(value.relocation, 'relocation'),
    coordinatorFence(value.coordinator),
    Buffer.of(relocationRole(value.senderRole)),
    actorAuthorityFence(value.actor),
    sessionRelocationOwnerFence(value.session)
  );
}

/** Decodes canonical service-wire command 42. */
export function decodeSessionRelocationSeal(frame: Uint8Array): ServiceSessionRelocationSeal {
  const reader = new Reader(frame);
  const command = reader.prefix();
  if (command.command !== M6bServiceWireCommand.sessionRelocationSeal || command.flags !== 0) {
    fail('Command is not sessionRelocationSeal.');
  }
  const relocation = reader.operationId('relocation');
  const coordinator = reader.coordinatorFence();
  const senderRole = reader.relocationRole();
  if (senderRole !== 'source' && senderRole !== 'coordinator') {
    fail('Session relocation seal sender role is invalid.');
  }
  const actor = reader.actorAuthorityFence();
  const session = reader.sessionRelocationOwnerFence();
  reader.end();
  return { relocation, coordinator, senderRole, actor, session };
}

/** Encodes canonical service-wire command 43. */
export function encodeSessionRelocationSealed(value: ServiceSessionRelocationSealed): Buffer {
  return concat(
    prefix(M6bServiceWireCommand.sessionRelocationSealed),
    wireId(value.relocation, 'relocation'),
    coordinatorFence(value.coordinator),
    actorAuthorityFence(value.actor),
    sessionRelocationOwnerFence(value.session)
  );
}

/** Decodes canonical service-wire command 43. */
export function decodeSessionRelocationSealed(frame: Uint8Array): ServiceSessionRelocationSealed {
  const reader = new Reader(frame);
  const command = reader.prefix();
  if (command.command !== M6bServiceWireCommand.sessionRelocationSealed || command.flags !== 0) {
    fail('Command is not sessionRelocationSealed.');
  }
  const relocation = reader.operationId('relocation');
  const coordinator = reader.coordinatorFence();
  const actor = reader.actorAuthorityFence();
  const session = reader.sessionRelocationOwnerFence();
  reader.end();
  return { relocation, coordinator, actor, session };
}

/** Encodes canonical service-wire command 44. */
export function encodeSessionRelocationRoute(value: ServiceSessionRelocationRoute): Buffer {
  const route = value.route.action === 'commit'
    ? (() => {
        if (value.senderRole !== 'target' && value.senderRole !== 'coordinator') {
          fail('Session relocation commit sender role is invalid.');
        }
        if (
          value.route.targetAuthorityOwnerGeneration
            <= value.route.previousAuthorityOwnerGeneration
        ) {
          fail('Session relocation commit must advance the authority owner generation.');
        }
        return concat(
          u64(value.route.previousAuthorityOwnerGeneration),
          u64(value.route.targetAuthorityOwnerGeneration),
          rid(value.route.targetNodeRid, 'targetNodeRid'),
          u64(value.route.targetNodeGeneration)
        );
      })()
    : (() => {
        if (value.senderRole !== 'source' && value.senderRole !== 'coordinator') {
          fail('Session relocation abort sender role is invalid.');
        }
        return u64(value.route.currentAuthorityOwnerGeneration);
      })();
  return concat(
    prefix(M6bServiceWireCommand.sessionRelocationRoute),
    wireId(value.relocation, 'relocation'),
    coordinatorFence(value.coordinator),
    Buffer.of(relocationRole(value.senderRole)),
    actorRef(value.actor),
    sessionRelocationOwnerFence(value.session),
    Buffer.of(value.route.action === 'commit' ? 1 : 2),
    u16(route.byteLength),
    route
  );
}

/** Decodes canonical service-wire command 44. */
export function decodeSessionRelocationRoute(frame: Uint8Array): ServiceSessionRelocationRoute {
  const reader = new Reader(frame);
  const command = reader.prefix();
  if (command.command !== M6bServiceWireCommand.sessionRelocationRoute || command.flags !== 0) {
    fail('Command is not sessionRelocationRoute.');
  }
  const relocation = reader.operationId('relocation');
  const coordinator = reader.coordinatorFence();
  const senderRole = reader.relocationRole();
  const actor = reader.actorRef('actor');
  const session = reader.sessionRelocationOwnerFence();
  const action = reader.u8('route.action');
  const routeLength = reader.u16('route.length');
  const routeEnd = reader.offset + routeLength;
  if (routeEnd > reader.bytes.byteLength) fail('Truncated Session relocation route body.');
  if (action === 1) {
    if (senderRole !== 'target' && senderRole !== 'coordinator') {
      fail('Session relocation commit sender role is invalid.');
    }
    const previousAuthorityOwnerGeneration = reader.nonZeroU64(
      'previousAuthorityOwnerGeneration'
    );
    const targetAuthorityOwnerGeneration = reader.nonZeroU64(
      'targetAuthorityOwnerGeneration'
    );
    const targetNodeRid = reader.rid('targetNodeRid');
    const targetNodeGeneration = reader.nonZeroU64('targetNodeGeneration');
    if (reader.offset !== routeEnd) fail('Invalid Session relocation commit route length.');
    if (targetAuthorityOwnerGeneration <= previousAuthorityOwnerGeneration) {
      fail('Session relocation commit must advance the authority owner generation.');
    }
    reader.end();
    return {
      relocation,
      coordinator,
      senderRole,
      actor,
      session,
      route: {
        action: 'commit',
        previousAuthorityOwnerGeneration,
        targetAuthorityOwnerGeneration,
        targetNodeRid,
        targetNodeGeneration
      }
    };
  }
  if (action === 2) {
    if (senderRole !== 'source' && senderRole !== 'coordinator') {
      fail('Session relocation abort sender role is invalid.');
    }
    const currentAuthorityOwnerGeneration = reader.nonZeroU64(
      'currentAuthorityOwnerGeneration'
    );
    if (reader.offset !== routeEnd) fail('Invalid Session relocation abort route length.');
    reader.end();
    return {
      relocation,
      coordinator,
      senderRole,
      actor,
      session,
      route: { action: 'abort', currentAuthorityOwnerGeneration }
    };
  }
  fail('Session relocation route action is invalid.');
}

export function encodeInstanceSpotHeader(
  route: ServiceInstanceRouteFence,
  sourceNodeGeneration: bigint,
  sourceNodeRid: string,
  sourceSpotId: string | undefined,
  operationKind: 'send' | 'request',
  operation: { readonly high: bigint; readonly low: bigint },
  replyRouteId?: bigint,
  hasMetadata = false
): Buffer {
  const routeBody = concat(
    rid(route.targetNodeRid, 'targetNodeRid'),
    u64(route.targetNodeGeneration),
    rid(route.targetSpotId, 'targetSpotId'),
    u64(route.objectGeneration),
    text8(route.ownerId, 'ownerId'),
    u64(route.authorityOwnerGeneration),
    u64(route.leaseGeneration),
    text16(route.storeVersion, 'storeVersion')
  );
  if (
    operationKind === 'send'
    && (operation.high !== 0n || operation.low !== 0n || replyRouteId !== undefined)
  ) {
    throw new RangeError('Instance Spot send must not carry an operation or reply route.');
  }
  if (operationKind === 'request' && operation.high === 0n && operation.low === 0n) {
    throw new RangeError('Instance Spot request requires a non-zero operation.');
  }
  return concat(
    prefix(
      M6bServiceWireCommand.instanceSpot,
      hasMetadata ? M6bServiceWireFlag.metadata : 0
    ),
    Buffer.of(1),
    u16(routeBody.byteLength),
    routeBody,
    u64Any(sourceNodeGeneration),
    rid(sourceNodeRid, 'sourceNodeRid'),
    optionalRid(sourceSpotId),
    Buffer.of(operationKind === 'send' ? 1 : 2),
    u64Any(operation.high),
    u64Any(operation.low),
    ...(operationKind === 'request'
      ? [u64(requirePositive(replyRouteId, 'replyRouteId'))]
      : [])
  );
}

export function encodeInstanceSpotActivationHeader(
  target: ServiceInstanceActivationTarget,
  sourceNodeGeneration: bigint,
  sourceNodeRid: string,
  sourceSpotId: string | undefined,
  operationKind: 'send' | 'request',
  operation: { readonly high: bigint; readonly low: bigint },
  deadlineUnixMs: bigint,
  replyRouteId?: bigint,
  hasMetadata = false
): Buffer {
  if (operation.high === 0n && operation.low === 0n) {
    throw new RangeError('Instance Spot activation requires a non-zero operation identity.');
  }
  if (deadlineUnixMs <= 0n) {
    throw new RangeError('Instance Spot activation requires a positive deadline.');
  }
  if (operationKind === 'send' && replyRouteId !== undefined) {
    throw new RangeError('Instance Spot send must not carry a reply route.');
  }
  const targetBody = concat(
    rid(target.targetNodeRid, 'targetNodeRid'),
    u64(target.targetNodeGeneration),
    rid(target.targetSpotId, 'targetSpotId'),
    text16(target.stableType, 'stableType'),
    text16(target.descriptorVersion, 'descriptorVersion')
  );
  return concat(
    prefix(
      M6bServiceWireCommand.instanceSpot,
      hasMetadata ? M6bServiceWireFlag.metadata : 0
    ),
    Buffer.of(2),
    u16(targetBody.byteLength),
    targetBody,
    u64Any(sourceNodeGeneration),
    rid(sourceNodeRid, 'sourceNodeRid'),
    optionalRid(sourceSpotId),
    Buffer.of(operationKind === 'send' ? 1 : 2),
    u64Any(operation.high),
    u64Any(operation.low),
    u64(deadlineUnixMs),
    ...(operationKind === 'request'
      ? [u64(requirePositive(replyRouteId, 'replyRouteId'))]
      : [])
  );
}

export function encodeUserSpotCreateHeader(record: Omit<ServiceUserSpotCreateRecord, 'kind'>): Buffer {
  if (record.operation.high === 0n && record.operation.low === 0n) {
    throw new RangeError('User Spot create requires a non-zero operation identity.');
  }
  const fence = record.reservation;
  if (fence.pendingCapacityDelta < 1) {
    throw new RangeError('User Spot create requires a positive pending capacity delta.');
  }
  return concat(
    prefix(M6bServiceWireCommand.userSpotCreate),
    u64(record.correlation),
    u64Any(record.operation.high),
    u64Any(record.operation.low),
    rid(record.sourceNodeRid, 'sourceNodeRid'),
    u64(record.sourceNodeGeneration),
    rid(record.spotId, 'spotId'),
    text8(record.stableType, 'stableType'),
    text8(fence.reservationId, 'reservationId'),
    text16(fence.expectedStoreVersion, 'expectedStoreVersion'),
    u64(fence.objectGeneration),
    u64(fence.authorityOwnerGeneration),
    rid(fence.targetNodeRid, 'targetNodeRid'),
    u64(fence.targetNodeGeneration),
    text8(fence.targetOwnerId, 'targetOwnerId'),
    u64(fence.targetOwnerLeaseGeneration),
    u32(fence.pendingCapacityDelta, 'pendingCapacityDelta'),
    u64(record.deadlineUnixMs)
  );
}

export function encodeActorCreateHeader(record: Omit<ServiceActorCreateRecord, 'kind'>): Buffer {
  if (record.operation.high === 0n && record.operation.low === 0n) {
    throw new RangeError('Actor create requires a non-zero operation identity.');
  }
  const fence = record.reservation;
  if (fence.pendingCapacityDelta !== 1) {
    throw new RangeError('Actor create requires a pending capacity delta of one.');
  }
  return concat(
    prefix(M6bServiceWireCommand.actorCreate),
    u64(record.correlation),
    u64Any(record.operation.high),
    u64Any(record.operation.low),
    rid(record.sourceNodeRid, 'sourceNodeRid'),
    u64(record.sourceNodeGeneration),
    text8(record.actorId, 'actorId'),
    text8(record.stableType, 'stableType'),
    text8(fence.reservationId, 'reservationId'),
    text16(fence.expectedStoreVersion, 'expectedStoreVersion'),
    u64(fence.objectGeneration),
    u64(fence.authorityOwnerGeneration),
    rid(fence.targetNodeRid, 'targetNodeRid'),
    u64(fence.targetNodeGeneration),
    text8(fence.targetOwnerId, 'targetOwnerId'),
    u64(fence.targetOwnerLeaseGeneration),
    u32(fence.pendingCapacityDelta, 'pendingCapacityDelta'),
    u64(record.deadlineUnixMs)
  );
}

export function encodeUserSpotCloseHeader(record: Omit<ServiceUserSpotCloseRecord, 'kind'>): Buffer {
  if (record.operation.high === 0n && record.operation.low === 0n) {
    throw new RangeError('User Spot close requires a non-zero operation identity.');
  }
  const fence = concat(
    rid(record.target.spotId, 'spotId'),
    u64(record.target.objectGeneration),
    rid(record.target.targetNodeRid, 'targetNodeRid'),
    u64(record.target.targetNodeGeneration),
    u64(record.target.authorityOwnerGeneration),
    text16(record.target.expectedStoreVersion, 'expectedStoreVersion')
  );
  return concat(
    prefix(M6bServiceWireCommand.userSpotClose),
    u64(record.correlation),
    u64Any(record.operation.high),
    u64Any(record.operation.low),
    rid(record.sourceNodeRid, 'sourceNodeRid'),
    u64(record.sourceNodeGeneration),
    Buffer.of(1),
    u16(fence.byteLength),
    fence,
    u64(record.deadlineUnixMs)
  );
}

export function encodeMessageFollowHeader(
  record: Omit<ServiceMessageFollowRecord, 'kind'>
): Buffer {
  validateMessageFollowRecord(record);
  const body = concat(
    messageFollowRoute(record.source),
    messageFollowRoute(record.target),
    Buffer.of(record.hopCount),
    u32(record.queuedMessages, 'queuedMessages'),
    u32(record.queuedBytes, 'queuedBytes'),
    u64Any(record.originalOperation.high),
    u64Any(record.originalOperation.low),
    u64Any(record.originalReplyRouteId)
  );
  if (body.byteLength > 16 * 1024 * 1024) {
    throw new RangeError('Message Follow body exceeds 16 MiB.');
  }
  return concat(
    prefix(M6bServiceWireCommand.messageFollow),
    Buffer.of(1),
    u32(body.byteLength, 'messageFollow.length'),
    body
  );
}

export function decodeStatefulHeader(frame: Uint8Array): ServiceStatefulWireRecord {
  const reader = new Reader(frame);
  const command = reader.prefix();
  switch (command.command) {
    case M6bServiceWireCommand.spotSend:
    case M6bServiceWireCommand.spotRequest: {
      requireFlags(command.flags, 0);
      const request = command.command === M6bServiceWireCommand.spotRequest;
      const correlation = request ? reader.nonZeroU64('correlation') : undefined;
      const sourceSpotId = reader.rid('sourceSpotId');
      const target = reader.directSpotFence();
      reader.end();
      return {
        kind: request ? 'spotRequest' : 'spotSend',
        ...(correlation === undefined ? {} : { correlation }),
        sourceSpotId,
        target
      };
    }
    case M6bServiceWireCommand.actorSend:
    case M6bServiceWireCommand.actorRequest: {
      const request = command.command === M6bServiceWireCommand.actorRequest;
      const hasBinding = command.flags !== 0;
      requireFlags(
        command.flags,
        hasBinding
          ? M6bServiceWireFlag.boundSession | M6bServiceWireFlag.sourceSpotId
          : 0
      );
      const correlation = request ? reader.nonZeroU64('correlation') : undefined;
      const sourceActor = reader.optionalActor();
      const target = reader.actorFence();
      const boundSession = hasBinding
        ? {
            sessionRid: reader.rid('sourceSessionRid'),
            bindingGeneration: reader.nonZeroU64('sourceBindingGeneration'),
            sequence: reader.nonZeroU64('sourceSessionSequence')
          }
        : undefined;
      reader.end();
      return {
        kind: request ? 'actorRequest' : 'actorSend',
        ...(correlation === undefined ? {} : { correlation }),
        ...(sourceActor === undefined ? {} : { sourceActor }),
        target,
        ...(boundSession === undefined ? {} : { boundSession })
      };
    }
    case M6bServiceWireCommand.logicalMulticast: {
      requireFlags(command.flags, 0);
      const result = {
        kind: 'logicalMulticast' as const,
        channelName: reader.text8('channelName'),
        topic: reader.text8('topic'),
        sourceSpotId: reader.rid('sourceSpotId')
      };
      reader.end();
      return result;
    }
    case M6bServiceWireCommand.actorLookup: {
      requireFlags(command.flags, 0);
      const result = {
        kind: 'actorLookup' as const,
        correlation: reader.nonZeroU64('correlation'),
        actorId: reader.text8('actorId')
      };
      reader.end();
      return result;
    }
    case M6bServiceWireCommand.actorDestroy: {
      requireFlags(command.flags, 0);
      const result = {
        kind: 'actorDestroy' as const,
        correlation: reader.nonZeroU64('correlation'),
        actor: reader.actorFence()
      };
      reader.end();
      return result;
    }
    case M6bServiceWireCommand.actorJoin: {
      requireFlags(command.flags, 0);
      const correlation = reader.nonZeroU64('correlation');
      const actor = reader.actorFence();
      const entry = reader.bool8('entry');
      const target = reader.spotFence();
      reader.end();
      return { kind: 'actorJoin', correlation, actor, entry, target };
    }
    case M6bServiceWireCommand.boundSessionSend: {
      requireFlags(command.flags, 0);
      const actor = reader.actorFence();
      const expectedBindingGeneration = reader.nonZeroU64('expectedBindingGeneration');
      reader.end();
      return { kind: 'boundSessionSend', actor, expectedBindingGeneration };
    }
    case M6bServiceWireCommand.boundSessionBind: {
      requireFlags(command.flags, 0);
      const correlation = reader.nonZeroU64('correlation');
      const actor = reader.actorFence();
      const sessionRid = reader.rid('sessionRid');
      const state = reader.u8('bindingState');
      const bodyLength = reader.u16('bindingBodyLength');
      if (bodyLength !== 8) fail('Binding transition body must contain one u64.');
      const generation = reader.nonZeroU64('bindingGeneration');
      reader.end();
      if (state !== 1 && state !== 2) fail('Unknown binding transition state.');
      return {
        kind: 'boundSessionBind',
        correlation,
        actor,
        sessionRid,
        binding: state === 1
          ? { state: 'active', generation }
          : { state: 'tombstone', retiredGeneration: generation }
      };
    }
    case M6bServiceWireCommand.boundSessionReplaced: {
      requireFlags(command.flags, 0);
      const actor = reader.actorRef('actorAuthority.actor');
      const targetNodeRid = reader.rid('actorAuthority.targetNodeRid');
      const actorAuthority = {
        actor: { ...actor, nodeRid: targetNodeRid },
        targetNodeGeneration: reader.nonZeroU64('actorAuthority.targetNodeGeneration'),
        authorityOwnerGeneration: reader.nonZeroU64(
          'actorAuthority.expectedAuthorityOwnerGeneration'
        ),
        ownerLeaseGeneration: reader.nonZeroU64(
          'actorAuthority.expectedOwnerLeaseGeneration'
        )
      };
      const retiredSession = {
        sessionOwnerNodeRid: reader.rid('sessionOwnerNodeRid'),
        sessionOwnerNodeGeneration: reader.nonZeroU64('sessionOwnerNodeGeneration'),
        sessionOwnerId: reader.text8('sessionOwnerId'),
        sessionOwnerLeaseGeneration: reader.nonZeroU64('sessionOwnerLeaseGeneration'),
        sessionRid: reader.rid('sessionRid'),
        retiredBindingGeneration: reader.nonZeroU64('retiredBindingGeneration')
      };
      reader.end();
      return { kind: 'boundSessionReplaced', actorAuthority, retiredSession };
    }
    case M6bServiceWireCommand.instanceSpot: {
      if ((command.flags & ~M6bServiceWireFlag.metadata) !== 0) {
        fail(`Invalid command flags '${command.flags}'.`);
      }
      const version = reader.u8('instanceRoute.version');
      if (version !== 1 && version !== 2) fail('Unsupported Instance route version.');
      const routeLength = reader.u16('instanceRoute.length');
      const routeEnd = reader.offset + routeLength;
      const commonTarget = {
        targetNodeRid: reader.rid('targetNodeRid'),
        targetNodeGeneration: reader.nonZeroU64('targetNodeGeneration'),
        targetSpotId: reader.rid('targetSpotId')
      };
      const route: ServiceInstanceRouteFence | undefined = version === 1
        ? {
            ...commonTarget,
            objectGeneration: reader.nonZeroU64('objectGeneration'),
            ownerId: reader.text8('ownerId'),
            authorityOwnerGeneration: reader.nonZeroU64('authorityOwnerGeneration'),
            leaseGeneration: reader.nonZeroU64('leaseGeneration'),
            storeVersion: reader.text16('storeVersion')
          }
        : undefined;
      const target: ServiceInstanceActivationTarget | undefined = version === 2
        ? {
            ...commonTarget,
            stableType: reader.text16('stableType'),
            descriptorVersion: reader.text16('descriptorVersion')
          }
        : undefined;
      if (reader.offset !== routeEnd) fail('Invalid Instance route body length.');
      const sourceNodeGeneration = reader.nonZeroU64('sourceNodeGeneration');
      const sourceNodeRid = reader.rid('sourceNodeRid');
      const sourceSpotId = reader.optionalRid('sourceSpotId');
      const operationValue = reader.u8('operationKind');
      if (operationValue !== 1 && operationValue !== 2) fail('Unknown Instance operation kind.');
      const operation = {
        high: reader.u64('operation.high'),
        low: reader.u64('operation.low')
      };
      const operationKind = operationValue === 1 ? 'send' as const : 'request' as const;
      if (version === 1 && (
        (operationKind === 'send' && (operation.high !== 0n || operation.low !== 0n))
        || (operationKind === 'request' && operation.high === 0n && operation.low === 0n)
      )) {
        fail('Invalid Instance operation identity.');
      }
      if (version === 2 && operation.high === 0n && operation.low === 0n) {
        fail('Instance activation requires a non-zero operation identity.');
      }
      const deadlineUnixMs = version === 2
        ? reader.nonZeroU64('deadlineUnixMs')
        : undefined;
      const replyRouteId = operationKind === 'request'
        ? reader.nonZeroU64('replyRouteId')
        : undefined;
      reader.end();
      const common = {
        kind: 'instanceSpot' as const,
        sourceNodeGeneration,
        sourceNodeRid,
        ...(sourceSpotId === undefined ? {} : { sourceSpotId }),
        operationKind,
        operation,
        ...(replyRouteId === undefined ? {} : { replyRouteId })
      };
      return version === 1
        ? { ...common, activation: 'ready', route: route! }
        : {
            ...common,
            activation: 'missing',
            target: target!,
            deadlineUnixMs: deadlineUnixMs!
          };
    }
    case M6bServiceWireCommand.userSpotCreate: {
      requireFlags(command.flags, 0);
      const correlation = reader.nonZeroU64('correlation');
      const operation = {
        high: reader.u64('operation.high'),
        low: reader.u64('operation.low')
      };
      if (operation.high === 0n && operation.low === 0n) {
        fail('User Spot create requires a non-zero operation identity.');
      }
      const record: ServiceUserSpotCreateRecord = {
        kind: 'userSpotCreate',
        correlation,
        operation,
        sourceNodeRid: reader.rid('sourceNodeRid'),
        sourceNodeGeneration: reader.nonZeroU64('sourceNodeGeneration'),
        spotId: reader.rid('spotId'),
        stableType: reader.text8('stableType'),
        reservation: {
          reservationId: reader.text8('reservationId'),
          expectedStoreVersion: reader.text16('expectedStoreVersion'),
          objectGeneration: reader.nonZeroU64('objectGeneration'),
          authorityOwnerGeneration: reader.nonZeroU64('authorityOwnerGeneration'),
          targetNodeRid: reader.rid('targetNodeRid'),
          targetNodeGeneration: reader.nonZeroU64('targetNodeGeneration'),
          targetOwnerId: reader.text8('targetOwnerId'),
          targetOwnerLeaseGeneration: reader.nonZeroU64('targetOwnerLeaseGeneration'),
          pendingCapacityDelta: reader.u32('pendingCapacityDelta')
        },
        deadlineUnixMs: reader.nonZeroU64('deadlineUnixMs')
      };
      if (record.reservation.pendingCapacityDelta === 0) {
        fail('User Spot create requires a positive pending capacity delta.');
      }
      reader.end();
      return record;
    }
    case M6bServiceWireCommand.actorCreate: {
      requireFlags(command.flags, 0);
      const correlation = reader.nonZeroU64('correlation');
      const operation = {
        high: reader.u64('operation.high'),
        low: reader.u64('operation.low')
      };
      if (operation.high === 0n && operation.low === 0n) {
        fail('Actor create requires a non-zero operation identity.');
      }
      const record: ServiceActorCreateRecord = {
        kind: 'actorCreate',
        correlation,
        operation,
        sourceNodeRid: reader.rid('sourceNodeRid'),
        sourceNodeGeneration: reader.nonZeroU64('sourceNodeGeneration'),
        actorId: reader.text8('actorId'),
        stableType: reader.text8('stableType'),
        reservation: {
          reservationId: reader.text8('reservationId'),
          expectedStoreVersion: reader.text16('expectedStoreVersion'),
          objectGeneration: reader.nonZeroU64('objectGeneration'),
          authorityOwnerGeneration: reader.nonZeroU64('authorityOwnerGeneration'),
          targetNodeRid: reader.rid('targetNodeRid'),
          targetNodeGeneration: reader.nonZeroU64('targetNodeGeneration'),
          targetOwnerId: reader.text8('targetOwnerId'),
          targetOwnerLeaseGeneration: reader.nonZeroU64('targetOwnerLeaseGeneration'),
          pendingCapacityDelta: reader.u32('pendingCapacityDelta')
        },
        deadlineUnixMs: reader.nonZeroU64('deadlineUnixMs')
      };
      if (record.reservation.pendingCapacityDelta !== 1) {
        fail('Actor create requires a pending capacity delta of one.');
      }
      reader.end();
      return record;
    }
    case M6bServiceWireCommand.userSpotClose: {
      requireFlags(command.flags, 0);
      const correlation = reader.nonZeroU64('correlation');
      const operation = {
        high: reader.u64('operation.high'),
        low: reader.u64('operation.low')
      };
      if (operation.high === 0n && operation.low === 0n) {
        fail('User Spot close requires a non-zero operation identity.');
      }
      const sourceNodeRid = reader.rid('sourceNodeRid');
      const sourceNodeGeneration = reader.nonZeroU64('sourceNodeGeneration');
      if (reader.u8('closeFence.version') !== 1) fail('Unsupported User Spot close fence version.');
      const fenceLength = reader.u16('closeFence.length');
      const fenceEnd = reader.offset + fenceLength;
      if (fenceEnd > reader.bytes.byteLength) fail('Truncated User Spot close fence.');
      const target = {
        spotId: reader.rid('spotId'),
        objectGeneration: reader.nonZeroU64('objectGeneration'),
        targetNodeRid: reader.rid('targetNodeRid'),
        targetNodeGeneration: reader.nonZeroU64('targetNodeGeneration'),
        authorityOwnerGeneration: reader.nonZeroU64('authorityOwnerGeneration'),
        expectedStoreVersion: reader.text16('expectedStoreVersion')
      };
      if (reader.offset !== fenceEnd) fail('Invalid User Spot close fence length.');
      const deadlineUnixMs = reader.nonZeroU64('deadlineUnixMs');
      reader.end();
      return {
        kind: 'userSpotClose',
        correlation,
        operation,
        sourceNodeRid,
        sourceNodeGeneration,
        target,
        deadlineUnixMs
      };
    }
    case M6bServiceWireCommand.messageFollow: {
      requireFlags(command.flags, 0);
      if (reader.u8('messageFollow.version') !== 1) {
        fail('Unsupported Message Follow version.');
      }
      const length = reader.u32('messageFollow.length');
      const end = reader.offset + length;
      if (length > 16 * 1024 * 1024 || end > reader.bytes.byteLength) {
        fail('Invalid Message Follow body length.');
      }
      const record: ServiceMessageFollowRecord = {
        kind: 'messageFollow',
        source: reader.messageFollowRoute('source'),
        target: reader.messageFollowRoute('target'),
        hopCount: reader.u8('hopCount'),
        queuedMessages: reader.u32('queuedMessages'),
        queuedBytes: reader.u32('queuedBytes'),
        originalOperation: reader.operationId('originalOperation'),
        originalReplyRouteId: reader.u64('originalReplyRouteId')
      };
      if (reader.offset !== end) fail('Invalid Message Follow body length.');
      reader.end();
      validateMessageFollowRecord(record, fail);
      return record;
    }
    default:
      fail(`Command '${command.command}' is not owned by the M6B codec.`);
  }
}

export function encodeStatefulReply(
  correlation: bigint,
  terminalResult: number,
  failureCode: number,
  tail?: ServiceStatefulReplyTail
): Buffer {
  const tailBytes = tail === undefined ? Buffer.alloc(0) : encodeReplyTail(tail);
  return concat(
    prefix(20),
    u64(correlation),
    u32(terminalResult, 'terminalResult'),
    u32(failureCode, 'failureCode'),
    tailBytes
  );
}

/** Encodes canonical maintenance relocation commands 30, 31, 34, and 40. */
export function encodeMaintenanceRelocationControl(
  value: ServiceMaintenanceRelocationControl
): Buffer {
  const base = concat(
    wireId(value.relocation, 'relocation'),
    u64(value.targetAttemptGeneration),
    coordinatorFence(value.coordinator)
  );
  switch (value.kind) {
    case 'prepare':
      return concat(
        prefix(M6bServiceWireCommand.relocationPrepare),
        wireId(value.relocation, 'relocation'),
        u64(value.targetAttemptGeneration),
        coordinatorFence(value.coordinator),
        relocationTarget(value.target),
        Buffer.of(relocationRole(value.initiatorRole)),
        relocationObject(value.object),
        rid(value.sourceNodeRid, 'sourceNodeRid'),
        u64(value.sourceNodeGeneration),
        payloadTotalLength(value.payloadTotalLength),
        payloadChunkCount(value.payloadChunkCount),
        u32(value.payloadChecksumCrc32c, 'payloadChecksumCrc32c'),
        u32(value.baseChecksumCrc32c, 'baseChecksumCrc32c'),
        applicationVersion(value.applicationVersion)
      );
    case 'ready':
      return concat(
        prefix(M6bServiceWireCommand.relocationReady),
        wireId(value.relocation, 'relocation'),
        u64(value.targetAttemptGeneration),
        coordinatorFence(value.coordinator),
        relocationTarget(value.target),
        relocationObject(value.object),
        Buffer.of(relocationRole(value.senderRole))
      );
    case 'failed':
      return concat(
        prefix(M6bServiceWireCommand.relocationFailed),
        wireId(value.relocation, 'relocation'),
        u64(value.targetAttemptGeneration),
        coordinatorFence(value.coordinator),
        relocationTarget(value.target),
        relocationObject(value.object),
        Buffer.of(relocationRole(value.senderRole)),
        u32(requiredNonzeroU32(value.failureCode, 'failureCode'), 'failureCode')
      );
    case 'data':
      return concat(
        prefix(M6bServiceWireCommand.relocationData),
        base,
        Buffer.of(relocationRole(value.senderRole)),
        relocationObject(value.object),
        encodeServiceWireFrozenRecord(value.frozenRecord)
      );
    case 'cutover':
      return concat(
        prefix(M6bServiceWireCommand.relocationCutover),
        base,
        Buffer.of(relocationRole(value.senderRole)),
        relocationObject(value.object),
        ordinalOrZero(value.boundaryRecordCount, 'boundaryRecordCount'),
        u32(value.boundaryChecksumCrc32c, 'boundaryChecksumCrc32c')
      );
    case 'state':
      return concat(
        prefix(M6bServiceWireCommand.relocationState),
        base,
        Buffer.of(relocationRole(value.senderRole)),
        relocationObject(value.object),
        Buffer.of(payloadStage(value.payloadStage)),
        u32(value.chunkOrdinal, 'chunkOrdinal'),
        relocationChunkData(value.chunkData)
      );
  }
}

/** Decodes canonical maintenance relocation commands and rejects malformed vectors. */
export function decodeMaintenanceRelocationControl(
  frame: Uint8Array
): ServiceMaintenanceRelocationControl {
  const reader = new Reader(frame);
  const prefixValue = reader.prefix();
  switch (prefixValue.command) {
    case M6bServiceWireCommand.relocationPrepare: {
      requireFlags(prefixValue.flags, 0);
      const relocation = reader.operationId('relocation');
      const targetAttemptGeneration = reader.nonZeroU64('targetAttemptGeneration');
      const coordinator = reader.coordinatorFence();
      const target = reader.relocationTarget();
      const initiatorRole = reader.relocationRole();
      const object = reader.relocationObject();
      const sourceNodeRid = reader.rid('sourceNodeRid');
      const sourceNodeGeneration = reader.nonZeroU64('sourceNodeGeneration');
      const payloadTotalLength = reader.payloadTotalLength();
      const payloadChunkCount = reader.payloadChunkCount();
      const payloadChecksumCrc32c = reader.u32('payloadChecksumCrc32c');
      const baseChecksumCrc32c = reader.u32('baseChecksumCrc32c');
      const applicationVersion = reader.applicationVersion();
      reader.end();
      return { kind: 'prepare', relocation, targetAttemptGeneration, coordinator,
        target, initiatorRole, object, sourceNodeRid, sourceNodeGeneration,
        payloadTotalLength, payloadChunkCount, payloadChecksumCrc32c,
        baseChecksumCrc32c, applicationVersion };
    }
    case M6bServiceWireCommand.relocationReady: {
      requireFlags(prefixValue.flags, 0);
      const relocation = reader.operationId('relocation');
      const targetAttemptGeneration = reader.nonZeroU64('targetAttemptGeneration');
      const coordinator = reader.coordinatorFence();
      const target = reader.relocationTarget();
      const object = reader.relocationObject();
      const senderRole = reader.relocationRole();
      reader.end();
      return { kind: 'ready', relocation, targetAttemptGeneration, coordinator,
        target, object, senderRole };
    }
    case M6bServiceWireCommand.relocationFailed: {
      requireFlags(prefixValue.flags, 0);
      const relocation = reader.operationId('relocation');
      const targetAttemptGeneration = reader.nonZeroU64('targetAttemptGeneration');
      const coordinator = reader.coordinatorFence();
      const target = reader.relocationTarget();
      const object = reader.relocationObject();
      const senderRole = reader.relocationRole();
      const failureCode = requiredNonzeroU32(
        reader.u32('failureCode'), 'failureCode');
      reader.end();
      return { kind: 'failed', relocation, targetAttemptGeneration, coordinator,
        target, object, senderRole, failureCode };
    }
    case M6bServiceWireCommand.relocationData: {
      requireFlags(prefixValue.flags, 0);
      const base = reader.relocationBase();
      const senderRole = reader.relocationRole();
      const object = reader.relocationObject();
      const frozenBytes = reader.takeRemaining();
      const frozenRecord = decodeServiceWireFrozenRecord(frozenBytes);
      reader.end();
      return { kind: 'data', ...base, senderRole, object, frozenRecord };
    }
    case M6bServiceWireCommand.relocationCutover: {
      requireFlags(prefixValue.flags, 0);
      const base = reader.relocationBase();
      const senderRole = reader.relocationRole();
      const object = reader.relocationObject();
      const boundaryRecordCount = reader.ordinal('boundaryRecordCount');
      const boundaryChecksumCrc32c = reader.u32('boundaryChecksumCrc32c');
      reader.end();
      return { kind: 'cutover', ...base, senderRole, object,
        boundaryRecordCount, boundaryChecksumCrc32c };
    }
    case M6bServiceWireCommand.relocationState: {
      requireFlags(prefixValue.flags, 0);
      const base = reader.relocationBase();
      const senderRole = reader.relocationRole();
      const object = reader.relocationObject();
      const stage = reader.payloadStage();
      const chunkOrdinal = reader.u32('chunkOrdinal');
      const chunkData = reader.relocationChunkData();
      reader.end();
      return { kind: 'state', ...base, senderRole, object, payloadStage: stage,
        chunkOrdinal, chunkData };
    }
    default:
      fail(`Unsupported maintenance relocation command '${prefixValue.command}'.`);
  }
}

/** Encodes canonical service-wire command 33 for a maintenance terminal relay. */
export function encodeMaintenanceReplyRelay(value: ServiceMaintenanceReplyRelay): Buffer {
  validateReplyTerminal(value.terminalResult, value.failureCode, value.payload !== undefined);
  const context = concat(
    wireId(value.relocation, 'relocation'),
    u64(value.targetAttemptGeneration),
    coordinatorFence(value.coordinator),
    u64(value.participantId),
    u64(value.sequence)
  );
  const payload = value.payload === undefined
    ? Buffer.alloc(0)
    : applicationPayload(value.payload);
  return concat(
    prefix(M6bServiceWireCommand.replyRelay),
    wireId(value.operation, 'operation'),
    u64(value.replyRouteId),
    Buffer.of(2),
    u16(context.byteLength),
    context,
    u32(value.terminalResult, 'terminalResult'),
    u32(value.failureCode, 'failureCode'),
    payload
  );
}

/** Decodes canonical service-wire command 33 and rejects non-maintenance contexts. */
export function decodeMaintenanceReplyRelay(frame: Uint8Array): ServiceMaintenanceReplyRelay {
  const reader = new Reader(frame);
  const command = reader.prefix();
  if (command.command !== M6bServiceWireCommand.replyRelay || command.flags !== 0) {
    fail('Invalid maintenance replyRelay prefix.');
  }
  const operation = reader.operationId('operation');
  const replyRouteId = reader.nonZeroU64('replyRouteId');
  if (reader.u8('contextKind') !== 2) fail('replyRelay context must be maintenanceRelocation.');
  const contextLength = reader.u16('contextLength');
  const contextEnd = reader.offset + contextLength;
  const relocation = reader.operationId('relocation');
  const targetAttemptGeneration = reader.nonZeroU64('targetAttemptGeneration');
  const coordinator = reader.coordinatorFence();
  const participantId = reader.nonZeroU64('participantId');
  const sequence = reader.nonZeroU64('sequence');
  if (reader.offset !== contextEnd) fail('Invalid maintenance replyRelay context length.');
  const terminalResult = reader.u32('terminalResult');
  const failureCode = reader.u32('failureCode');
  const payload = reader.remaining === 0 ? undefined : reader.applicationPayload();
  validateReplyTerminal(terminalResult, failureCode, payload !== undefined);
  reader.end();
  return {
    relocation,
    targetAttemptGeneration,
    coordinator,
    operation,
    replyRouteId,
    participantId,
    sequence,
    terminalResult,
    failureCode,
    ...(payload === undefined ? {} : { payload })
  };
}

/** Encodes canonical service-wire command 46. */
export function encodeMaintenanceReplyRelayAck(value: ServiceMaintenanceReplyRelayAck): Buffer {
  return concat(
    prefix(M6bServiceWireCommand.replyRelayAck),
    wireId(value.relocation, 'relocation'),
    coordinatorFence(value.coordinator),
    wireId(value.operation, 'operation'),
    u64(value.replyRouteId),
    requestSourceFence(value.requestSource),
    Buffer.of(value.status === 'terminalReceived' ? 1 : 2)
  );
}

/** Decodes canonical service-wire command 46. */
export function decodeMaintenanceReplyRelayAck(frame: Uint8Array): ServiceMaintenanceReplyRelayAck {
  const reader = new Reader(frame);
  const command = reader.prefix();
  if (command.command !== M6bServiceWireCommand.replyRelayAck || command.flags !== 0) {
    fail('Invalid maintenance replyRelayAck prefix.');
  }
  const relocation = reader.operationId('relocation');
  const coordinator = reader.coordinatorFence();
  const operation = reader.operationId('operation');
  const replyRouteId = reader.nonZeroU64('replyRouteId');
  const requestSource = reader.requestSourceFence();
  const statusValue = reader.u8('status');
  if (statusValue !== 1 && statusValue !== 2) fail('Invalid replyRelayAck status.');
  reader.end();
  return {
    relocation,
    coordinator,
    operation,
    replyRouteId,
    requestSource,
    status: statusValue === 1 ? 'terminalReceived' : 'alreadyTerminal'
  };
}

export function decodeStatefulReply(
  frame: Uint8Array,
  expectedCorrelation: bigint,
  operationKind: 'spotRequest' | 'actorRequest' | 'actorLookup' | 'actorDestroy' | 'actorJoin' | 'streamBind'
    | 'streamUnbind'
    | 'instanceSpotRequest' | 'userSpotCreate' | 'userSpotClose' | 'actorCreate',
  hasPayload = false
): ServiceStatefulReply {
  const reader = new Reader(frame);
  const command = reader.prefix();
  if (command.command !== 20 || command.flags !== 0) fail('Invalid stateful reply prefix.');
  const correlation = reader.nonZeroU64('correlation');
  if (correlation !== expectedCorrelation) fail('Stateful reply correlation mismatch.');
  const terminalResult = reader.u32('terminalResult');
  const failureCode = reader.u32('failureCode');
  validateReplyTerminal(terminalResult, failureCode, hasPayload);
  let tail: ServiceStatefulReplyTail | undefined;
  if (terminalResult === 0) {
    if (operationKind === 'actorLookup') {
      tail = {
        kind: 'actorLookup',
        actor: reader.actorRef('actor'),
        spot: {
          spotId: reader.rid('spotId'),
          generation: reader.nonZeroU64('spotGeneration')
        },
        membershipEpoch: reader.nonZeroU64('membershipEpoch'),
        authorityOwnerGeneration: reader.nonZeroU64('authorityOwnerGeneration')
      };
    } else if (operationKind === 'actorJoin') {
      const joinResult = reader.u32('joinResult');
      if (joinResult !== 0 && joinResult !== 1) fail('Invalid actor join result.');
      const bodyLength = reader.u16('joinBodyLength');
      const bodyEnd = reader.offset + bodyLength;
      let spot: ServiceSpotRef | undefined;
      if (joinResult === 0) {
        spot = reader.spotRef();
        const membershipEpoch = reader.nonZeroU64('membershipEpoch');
        // Tolerant of frames from an unpatched encoder that stops after
        // membershipEpoch: 0 means "not advertised".
        const receiveChunkLimitBytes = reader.offset === bodyEnd
          ? 0
          : (() => {
              const value = reader.u32('receiveChunkLimitBytes');
              if (value > RELOCATION_STATE_CHUNK_DATA_MAX_BYTES) {
                fail('receiveChunkLimitBytes exceeds the relocation state chunk data bound.');
              }
              return value;
            })();
        if (reader.offset !== bodyEnd) fail('Invalid actor join body length.');
        tail = { kind: 'actorJoin', joinResult, spot, membershipEpoch, receiveChunkLimitBytes };
      } else {
        const hasSpot = reader.bool8('hasSpot');
        const optionalLength = reader.u16('optionalSpotLength');
        const optionalEnd = reader.offset + optionalLength;
        if (hasSpot) spot = reader.spotRef();
        if (reader.offset !== optionalEnd) fail('Invalid optional Spot body length.');
        if (reader.offset !== bodyEnd) fail('Invalid actor join body length.');
        tail = { kind: 'actorJoin', joinResult, ...(spot === undefined ? {} : { spot }) };
      }
    } else if (operationKind === 'streamBind') {
      tail = {
        kind: 'streamBind',
        bindingGeneration: reader.nonZeroU64('bindingGeneration'),
        authorityOwnerGeneration: reader.nonZeroU64('authorityOwnerGeneration')
      };
    } else if (operationKind === 'userSpotCreate') {
      const createResult = reader.u8('createResult');
      if (createResult < 1 || createResult > 3) fail('Invalid User Spot create result.');
      tail = {
        kind: 'userSpotCreate',
        createResult: (['existing', 'created', 'rejected'] as const)[createResult - 1]!,
        spotId: reader.rid('spotId'),
        objectGeneration: reader.nonZeroU64('objectGeneration')
      };
    } else if (operationKind === 'userSpotClose') {
      tail = {
        kind: 'userSpotClose',
        closed: reader.bool8('closed')
      };
    } else if (operationKind === 'actorCreate') {
      const createResult = reader.u8('createResult');
      if (createResult < 1 || createResult > 3) fail('Invalid Actor create result.');
      const length = reader.u16('creationLength');
      const end = reader.offset + length;
      if (end > reader.bytes.byteLength) fail('Truncated Actor create terminal.');
      const result = (['existing', 'created', 'rejected'] as const)[createResult - 1]!;
      const actor = result === 'rejected'
        ? undefined
        : {
            nodeRid: reader.rid('actor.nodeRid'),
            actorId: reader.text8('actor.actorId'),
            generation: reader.nonZeroU64('actor.objectGeneration')
          };
      if (reader.offset !== end) fail('Invalid Actor create terminal length.');
      tail = {
        kind: 'actorCreate',
        createResult: result,
        ...(actor === undefined ? {} : { actor })
      };
    }
  }
  reader.end();
  return {
    correlation,
    terminalResult,
    failureCode,
    ...(tail === undefined ? {} : { tail })
  };
}

export function sessionBindingFromWire(
  actor: ServiceActorRef,
  sessionRid: string,
  sessionOwnerNodeRid: string,
  generation: bigint,
  membershipEpoch: bigint,
  sessionOwnerNodeGeneration?: bigint,
  sessionOwnerId?: string,
  sessionOwnerLeaseGeneration?: bigint
): ServiceSessionBinding {
  return {
    actor,
    sessionRid,
    sessionOwnerNodeRid,
    ...(sessionOwnerNodeGeneration === undefined ? {} : { sessionOwnerNodeGeneration }),
    ...(sessionOwnerId === undefined ? {} : { sessionOwnerId }),
    ...(sessionOwnerLeaseGeneration === undefined ? {} : { sessionOwnerLeaseGeneration }),
    bindingGeneration: generation,
    membershipEpoch
  };
}

function encodeReplyTail(tail: ServiceStatefulReplyTail): Buffer {
  switch (tail.kind) {
    case 'actorLookup':
      return concat(
        actorRef(tail.actor),
        rid(tail.spot.spotId, 'spotId'),
        u64(tail.spot.generation),
        u64(tail.membershipEpoch),
        u64(tail.authorityOwnerGeneration)
      );
    case 'actorJoin': {
      if (tail.joinResult === 0) {
        if (tail.spot === undefined) fail('Accepted actor join requires a SpotRef.');
        if (tail.membershipEpoch === undefined) {
          fail('Accepted actor join requires a membership epoch.');
        }
        const receiveChunkLimitBytes = tail.receiveChunkLimitBytes ?? 0;
        if (receiveChunkLimitBytes < 0
          || receiveChunkLimitBytes > RELOCATION_STATE_CHUNK_DATA_MAX_BYTES) {
          fail('receiveChunkLimitBytes exceeds the relocation state chunk data bound.');
        }
        const body = concat(
          spotRef(tail.spot),
          u64(tail.membershipEpoch),
          u32(receiveChunkLimitBytes, 'receiveChunkLimitBytes')
        );
        return concat(u32(0, 'joinResult'), u16(body.byteLength), body);
      }
      const optional = tail.spot === undefined
        ? concat(Buffer.of(0), u16(0))
        : (() => {
            const body = spotRef(tail.spot);
            return concat(Buffer.of(1), u16(body.byteLength), body);
          })();
      return concat(u32(1, 'joinResult'), u16(optional.byteLength), optional);
    }
    case 'streamBind':
      return concat(u64(tail.bindingGeneration), u64(tail.authorityOwnerGeneration));
    case 'userSpotCreate':
      return concat(
        Buffer.of((['existing', 'created', 'rejected'] as const).indexOf(tail.createResult) + 1),
        rid(tail.spotId, 'spotId'),
        u64(tail.objectGeneration)
      );
    case 'userSpotClose':
      return Buffer.of(tail.closed ? 1 : 0);
    case 'actorCreate': {
      const selected = tail.actor === undefined
        ? Buffer.alloc(0)
        : concat(
            rid(tail.actor.nodeRid, 'actor.nodeRid'),
            text8(tail.actor.actorId, 'actor.actorId'),
            u64(tail.actor.generation)
          );
      return concat(
        Buffer.of((['existing', 'created', 'rejected'] as const).indexOf(tail.createResult) + 1),
        u16(selected.byteLength),
        selected
      );
    }
  }
}

function wireId(value: ServiceWireOperationId, name: string): Buffer {
  if (value.high === 0n && value.low === 0n) {
    throw new RangeError(`${name} must not be zero.`);
  }
  return concat(u64Any(value.high), u64Any(value.low));
}

function validateReplyTerminal(
  terminalResult: number,
  failureCode: number,
  hasPayload: boolean
): void {
  if (hasPayload && terminalResult !== 0) {
    throw new RangeError('Failed stateful reply must not carry a payload.');
  }
  if (!isCanonicalWireReplyTerminal(terminalResult, failureCode)) {
    throw new RangeError('Stateful reply terminalResult does not match its framework failureCode.');
  }
}

function coordinatorFence(value: ServiceWireRelocationCoordinatorFence): Buffer {
  return concat(
    text8(value.ownerId, 'coordinatorOwnerId'),
    u64(value.leaseGeneration),
    rid(value.nodeRid, 'coordinatorNodeRid'),
    u64(value.nodeGeneration),
    text16(value.expectedAuthorityStoreVersion, 'expectedAuthorityStoreVersion')
  );
}

function relocationRole(value: ServiceWireRelocationRole): number {
  return value === 'source' ? 1 : value === 'target' ? 2 : 3;
}

function payloadStage(value: ServiceWireRelocationPayloadStage): number {
  return value === 'base' ? 0 : 1;
}

function applicationVersion(value: bigint): Buffer {
  if (value < 0n || value > 0x7fff_ffff_ffff_ffffn) {
    throw new RangeError('applicationVersion must be a non-negative i64.');
  }
  return u64Any(value);
}

function relocationTarget(value: ServiceWireRelocationTarget): Buffer {
  return concat(
    rid(value.nodeRid, 'targetNodeRid'),
    u64(value.nodeGeneration),
    text8(value.ownerId, 'targetOwnerId'),
    u64(value.ownerLeaseGeneration)
  );
}

function relocationObject(value: ServiceWireRelocationObject): Buffer {
  const body = value.kind === 'actor'
    ? concat(
        text8(value.actorId, 'actorId'),
        u64(value.objectGeneration),
        u64(value.expectedAuthorityOwnerGeneration)
      )
    : value.kind === 'userSpot'
      ? concat(
          rid(value.spotId, 'spotId'),
          u64(value.objectGeneration),
          u64(value.expectedAuthorityOwnerGeneration)
        )
      : concat(
          text8(value.stableType, 'instanceType'),
          text8(value.spotId, 'spotId'),
          u64(value.objectGeneration)
        );
  return concat(Buffer.of(value.kind === 'actor' ? 1 : value.kind === 'userSpot' ? 2 : 3),
    u16(body.byteLength), body);
}

function payloadTotalLength(value: bigint): Buffer {
  if (value < 0n || value > RELOCATION_PAYLOAD_TOTAL_LENGTH_MAX) {
    throw new RangeError('payloadTotalLength exceeds the relocation logical byte bound.');
  }
  return u64Any(value);
}

function payloadChunkCount(value: number): Buffer {
  if (!Number.isInteger(value) || value < 0
    || value > RELOCATION_PAYLOAD_CHUNK_COUNT_MAX) {
    throw new RangeError('payloadChunkCount exceeds the relocation chunk count bound.');
  }
  return u32(value, 'payloadChunkCount');
}

function requiredNonzeroU32(value: number, name: string): number {
  if (!Number.isInteger(value) || value <= 0 || value > 0xffff_ffff) {
    throw new RangeError(`${name} must be a non-zero u32.`);
  }
  return value;
}

function ordinalOrZero(value: bigint, name: string): Buffer {
  if (value < 0n || value > 0x7fff_ffff_ffff_ffffn) {
    throw new RangeError(`${name} must be a non-negative ordinal.`);
  }
  return u64Any(value);
}

function relocationChunkData(value: Uint8Array): Buffer {
  if (value.byteLength > RELOCATION_STATE_CHUNK_DATA_MAX_BYTES) {
    throw new RangeError('relocationState chunkData exceeds the chunk byte bound.');
  }
  return concat(u32(value.byteLength, 'chunkData.length'), value);
}


function validateRelocationPhase(value: number): void {
  const phases = ['none', 'preparing', 'captured', 'prepared', 'committed',
    'activating', 'activated', 'cleaning', 'completed', 'aborted'] as const;
  if (!Number.isInteger(value) || value < 0 || value >= phases.length) {
    fail('Invalid relocationData control phase.');
  }
}

function requestSourceFence(value: ServiceWireRequestSourceFence): Buffer {
  return concat(
    text8(value.ownerId, 'sourceOwnerId'),
    u64(value.leaseGeneration),
    rid(value.nodeRid, 'sourceNodeRid'),
    u64(value.nodeGeneration)
  );
}

function applicationPayload(value: NonNullable<ServiceMaintenanceReplyRelay['payload']>): Buffer {
  const body = concat(
    text8(value.packetName, 'packetName'),
    text8(value.contentType, 'contentType'),
    u32(value.bytes.byteLength, 'payload.length'),
    value.bytes
  );
  return concat(Buffer.of(1), u32(body.byteLength, 'applicationPayload.length'), body);
}

function prefix(command: number, flags = 0): Buffer {
  return Buffer.from([MAGIC_0, MAGIC_1, MAJOR, command, flags]);
}

function actorFence(value: ServiceActorRouteFence): Buffer {
  return concat(
    actorRef(value.actor),
    rid(value.actor.nodeRid, 'targetNodeRid'),
    u64(value.targetNodeGeneration),
    u64(value.authorityOwnerGeneration),
    u64(value.ownerLeaseGeneration)
  );
}

function actorAuthorityFence(value: ServiceBoundSessionActorAuthority): Buffer {
  return concat(
    actorRef(value.actor),
    rid(value.actor.nodeRid, 'targetNodeRid'),
    u64(value.targetNodeGeneration),
    u64(value.authorityOwnerGeneration),
    u64(value.ownerLeaseGeneration)
  );
}

function sessionRelocationOwnerFence(value: ServiceSessionRelocationOwnerFence): Buffer {
  return concat(
    rid(value.sessionOwnerNodeRid, 'sessionOwnerNodeRid'),
    u64(value.sessionOwnerNodeGeneration),
    text8(value.sessionOwnerId, 'sessionOwnerId'),
    u64(value.sessionOwnerLeaseGeneration),
    rid(value.sessionRid, 'sessionRid'),
    u64(value.bindingGeneration)
  );
}

function spotFence(value: ServiceSpotRouteFence): Buffer {
  return concat(
    spotRef(value.spot),
    rid(value.targetNodeRid, 'targetNodeRid'),
    u64(value.targetNodeGeneration),
    u64(value.authorityOwnerGeneration)
  );
}

function directSpotFence(value: ServiceDirectSpotRouteFence): Buffer {
  return concat(
    spotFence(value),
    u64(value.ownerLeaseGeneration),
    text16(value.storeVersion, 'storeVersion')
  );
}

function messageFollowRoute(value: ServiceMessageFollowRoute): Buffer {
  const body = value.kind === 'actor'
    ? concat(
        actorRef(value.actor),
        rid(value.targetNodeRid, 'targetNodeRid'),
        u64(value.targetNodeGeneration),
        u64(value.authorityOwnerGeneration),
        u64(value.ownerLeaseGeneration)
      )
    : concat(
        spotRef(value.spot),
        rid(value.targetNodeRid, 'targetNodeRid'),
        u64(value.targetNodeGeneration),
        u64(value.authorityOwnerGeneration),
        u64(value.ownerLeaseGeneration)
      );
  return concat(Buffer.of(value.kind === 'actor' ? 1 : 2), u16(body.byteLength), body);
}

function validateMessageFollowRecord(
  value: Omit<ServiceMessageFollowRecord, 'kind'>,
  invalid: (message: string) => never = (message) => { throw new RangeError(message); }
): void {
  if (value.source.kind !== value.target.kind) {
    invalid('Message Follow source and target object kinds differ.');
  }
  const sameObject = value.source.kind === 'actor' && value.target.kind === 'actor'
    ? value.source.actor.actorId === value.target.actor.actorId
      && value.source.actor.generation === value.target.actor.generation
    : value.source.kind === 'spot' && value.target.kind === 'spot'
      ? value.source.spot.spotId === value.target.spot.spotId
        && value.source.spot.generation === value.target.spot.generation
      : false;
  if (!sameObject) invalid('Message Follow source and target object identities differ.');
  if (!Number.isInteger(value.hopCount) || value.hopCount < 1 || value.hopCount > 8) {
    invalid('Message Follow hopCount must be in 1..8.');
  }
  if (!Number.isInteger(value.queuedMessages)
      || value.queuedMessages < 0
      || value.queuedMessages > 0xffff_ffff) {
    invalid('Message Follow queuedMessages must be a u32.');
  }
  if (!Number.isInteger(value.queuedBytes)
      || value.queuedBytes < 0
      || value.queuedBytes > 0xffff_ffff) {
    invalid('Message Follow queuedBytes must be a u32.');
  }
  if (value.originalOperation.high === 0n && value.originalOperation.low === 0n) {
    invalid('Message Follow originalOperation must not be zero.');
  }
  if (value.originalReplyRouteId < 0n
      || value.originalReplyRouteId > 0xffff_ffff_ffff_ffffn) {
    invalid('Message Follow originalReplyRouteId must be a u64.');
  }
}

function actorRef(value: ServiceActorRef): Buffer {
  return concat(text8(value.actorId, 'actorId'), u64(value.generation));
}

function optionalActor(value: ServiceActorRef | undefined): Buffer {
  return value === undefined
    ? Buffer.of(0)
    : concat(text8(value.actorId, 'sourceActorId'), u64(value.generation));
}

function spotRef(value: ServiceSpotRef): Buffer {
  return concat(rid(value.spotId, 'spotId'), u64(value.generation));
}

function rid(value: string, name: string): Buffer {
  const bytes = encodeServiceWireRoutingId(value, name, 0xff, outOfRange);
  return concat(Buffer.of(bytes.byteLength), bytes);
}

function text8(value: string, name: string): Buffer {
  return sized8(value, name);
}

function text16(value: string, name: string): Buffer {
  const bytes = encodeCanonicalServiceWireText(value, name, 0xffff, outOfRange);
  return concat(u16(bytes.byteLength), bytes);
}

function optionalRid(value: string | undefined): Buffer {
  return value === undefined ? Buffer.of(0) : rid(value, 'sourceSpotId');
}

function sized8(value: string, name: string): Buffer {
  const bytes = encodeCanonicalServiceWireText(value, name, 0xff, outOfRange);
  return concat(Buffer.of(bytes.byteLength), bytes);
}

function u16(value: number): Buffer {
  if (!Number.isInteger(value) || value < 0 || value > 0xffff) fail('u16 value is out of range.');
  const result = Buffer.alloc(2);
  result.writeUInt16BE(value);
  return result;
}

function u32(value: number, name: string): Buffer {
  if (!Number.isInteger(value) || value < 0 || value > 0xffff_ffff) {
    throw new RangeError(`${name} must be a u32.`);
  }
  const result = Buffer.alloc(4);
  result.writeUInt32BE(value);
  return result;
}

function u64(value: bigint): Buffer {
  requirePositive(value, 'u64');
  const result = Buffer.alloc(8);
  result.writeBigUInt64BE(value);
  return result;
}

function u64Any(value: bigint): Buffer {
  if (value < 0n || value > 0xffff_ffff_ffff_ffffn) {
    throw new RangeError('u64 value is out of range.');
  }
  const result = Buffer.alloc(8);
  result.writeBigUInt64BE(value);
  return result;
}

function requirePositive(value: bigint | undefined, name: string): bigint {
  if (value === undefined || value < 1n || value > 0xffff_ffff_ffff_ffffn) {
    throw new RangeError(`${name} must be a non-zero u64.`);
  }
  return value;
}

function requireFlags(actual: number, expected: number): void {
  if (actual !== expected) fail(`Invalid command flags '${actual}'.`);
}

function concat(...parts: readonly Uint8Array[]): Buffer {
  return Buffer.concat(parts.map(part => Buffer.from(part)));
}

function fail(message: string): never {
  throw new ServiceWireProtocolError(message);
}

function outOfRange(message: string): never {
  throw new RangeError(message);
}

class Reader {
  readonly bytes: Buffer;
  offset = 0;

  constructor(value: Uint8Array) {
    this.bytes = Buffer.from(value);
  }

  get remaining(): number {
    return this.bytes.byteLength - this.offset;
  }

  takeRemaining(): Buffer {
    const value = Buffer.from(this.bytes.subarray(this.offset));
    this.offset = this.bytes.byteLength;
    return value;
  }

  prefix(): { readonly command: number; readonly flags: number } {
    this.need(PREFIX_SIZE, 'prefix');
    if (
      this.bytes[this.offset] !== MAGIC_0
      || this.bytes[this.offset + 1] !== MAGIC_1
      || this.bytes[this.offset + 2] !== MAJOR
    ) {
      fail('Invalid service wire prefix.');
    }
    const command = this.bytes[this.offset + 3]!;
    const flags = this.bytes[this.offset + 4]!;
    this.offset += PREFIX_SIZE;
    return { command, flags };
  }

  u8(name: string): number {
    this.need(1, name);
    return this.bytes[this.offset++]!;
  }

  bool8(name: string): boolean {
    const value = this.u8(name);
    if (value !== 0 && value !== 1) fail(`${name} must be bool8.`);
    return value === 1;
  }

  u16(name: string): number {
    this.need(2, name);
    const value = this.bytes.readUInt16BE(this.offset);
    this.offset += 2;
    return value;
  }

  u32(name: string): number {
    this.need(4, name);
    const value = this.bytes.readUInt32BE(this.offset);
    this.offset += 4;
    return value;
  }

  nonZeroU64(name: string): bigint {
    this.need(8, name);
    const value = this.bytes.readBigUInt64BE(this.offset);
    this.offset += 8;
    if (value === 0n) fail(`${name} must be a non-zero u64.`);
    return value;
  }

  u64(name: string): bigint {
    this.need(8, name);
    const value = this.bytes.readBigUInt64BE(this.offset);
    this.offset += 8;
    return value;
  }

  operationId(name: string): ServiceWireOperationId {
    const value = {
      high: this.u64(`${name}.high`),
      low: this.u64(`${name}.low`)
    };
    if (value.high === 0n && value.low === 0n) fail(`${name} must not be zero.`);
    return value;
  }

  coordinatorFence(): ServiceWireRelocationCoordinatorFence {
    return {
      ownerId: this.text8('coordinatorOwnerId'),
      leaseGeneration: this.nonZeroU64('coordinatorLeaseGeneration'),
      nodeRid: this.rid('coordinatorNodeRid'),
      nodeGeneration: this.nonZeroU64('coordinatorNodeGeneration'),
      expectedAuthorityStoreVersion: this.text16('expectedAuthorityStoreVersion')
    };
  }

  requestSourceFence(): ServiceWireRequestSourceFence {
    return {
      ownerId: this.text8('sourceOwnerId'),
      leaseGeneration: this.nonZeroU64('sourceOwnerLeaseGeneration'),
      nodeRid: this.rid('sourceNodeRid'),
      nodeGeneration: this.nonZeroU64('sourceNodeGeneration')
    };
  }

  relocationBase(): ServiceWireRelocationBase {
    return {
      relocation: this.operationId('relocation'),
      targetAttemptGeneration: this.nonZeroU64('targetAttemptGeneration'),
      coordinator: this.coordinatorFence()
    };
  }

  relocationRole(): ServiceWireRelocationRole {
    const value = this.u8('relocationRole');
    if (value < 1 || value > 3) fail('Invalid relocationRole.');
    return value === 1 ? 'source' : value === 2 ? 'target' : 'coordinator';
  }

  ordinal(name: string): bigint {
    const value = this.u64(name);
    if (value > 0x7fff_ffff_ffff_ffffn) fail(`${name} exceeds the signed ordinal range.`);
    return value;
  }

  applicationVersion(): bigint {
    return this.ordinal('applicationVersion');
  }

  relocationTarget(): ServiceWireRelocationTarget {
    return {
      nodeRid: this.rid('targetNodeRid'),
      nodeGeneration: this.nonZeroU64('targetNodeGeneration'),
      ownerId: this.text8('targetOwnerId'),
      ownerLeaseGeneration: this.nonZeroU64('targetOwnerLeaseGeneration')
    };
  }

  relocationObject(): ServiceWireRelocationObject {
    const kind = this.u8('objectKind');
    const length = this.u16('objectLength');
    const end = this.offset + length;
    if (end > this.bytes.byteLength) fail('Truncated relocation object.');
    let value: ServiceWireRelocationObject;
    if (kind === 1) {
      value = { kind: 'actor', actorId: this.text8('actorId'),
        objectGeneration: this.nonZeroU64('actorGeneration'),
        expectedAuthorityOwnerGeneration: this.nonZeroU64('expectedAuthorityOwnerGeneration') };
    } else if (kind === 2) {
      value = { kind: 'userSpot', spotId: this.rid('spotId'),
        objectGeneration: this.nonZeroU64('spotGeneration'),
        expectedAuthorityOwnerGeneration: this.nonZeroU64('expectedAuthorityOwnerGeneration') };
    } else if (kind === 3) {
      value = { kind: 'instanceSpot', stableType: this.text8('instanceType'),
        spotId: this.text8('spotId'), objectGeneration: this.nonZeroU64('objectGeneration') };
    } else {
      fail('Invalid relocation object kind.');
    }
    if (this.offset !== end) fail('Invalid relocation object length.');
    return value;
  }

  payloadTotalLength(): bigint {
    const value = this.u64('payloadTotalLength');
    if (value > RELOCATION_PAYLOAD_TOTAL_LENGTH_MAX) {
      fail('payloadTotalLength exceeds the relocation logical byte bound.');
    }
    return value;
  }

  payloadChunkCount(): number {
    const value = this.u32('payloadChunkCount');
    if (value > RELOCATION_PAYLOAD_CHUNK_COUNT_MAX) {
      fail('payloadChunkCount exceeds the relocation chunk count bound.');
    }
    return value;
  }

  payloadStage(): ServiceWireRelocationPayloadStage {
    const value = this.u8('payloadStage');
    if (value !== 0 && value !== 1) fail('Invalid relocation payloadStage.');
    return value === 0 ? 'base' : 'final';
  }

  relocationChunkData(): Buffer {
    const length = this.u32('chunkData.length');
    if (length > RELOCATION_STATE_CHUNK_DATA_MAX_BYTES) {
      fail('relocationState chunkData exceeds the chunk byte bound.');
    }
    this.need(length, 'chunkData');
    const value = Buffer.from(this.bytes.subarray(this.offset, this.offset + length));
    this.offset += length;
    return value;
  }

  applicationPayload(): NonNullable<ServiceMaintenanceReplyRelay['payload']> {
    if (this.u8('applicationPayload.version') !== 1) {
      fail('Unsupported application payload envelope version.');
    }
    const bodyLength = this.u32('applicationPayload.length');
    const bodyEnd = this.offset + bodyLength;
    if (bodyEnd > this.bytes.byteLength) fail('Truncated application payload envelope.');
    const packetName = this.text8('packetName');
    const contentType = this.text8('contentType');
    const payloadLength = this.u32('payload.length');
    this.need(payloadLength, 'payload');
    const bytes = Buffer.from(this.bytes.subarray(this.offset, this.offset + payloadLength));
    this.offset += payloadLength;
    if (this.offset !== bodyEnd) fail('Invalid application payload envelope length.');
    return { packetName, contentType, bytes };
  }

  rid(name: string): string {
    return this.opaque8(name);
  }

  text8(name: string): string {
    return this.sized8(name);
  }

  text16(name: string): string {
    const length = this.u16(`${name}.length`);
    if (length === 0) fail(`${name} must not be empty.`);
    this.need(length, name);
    const bytes = this.bytes.subarray(this.offset, this.offset + length);
    this.offset += length;
    return decodeCanonicalServiceWireText(bytes, name, fail);
  }

  optionalRid(name: string): string | undefined {
    const length = this.u8(`${name}.length`);
    if (length === 0) return undefined;
    this.need(length, name);
    const bytes = this.bytes.subarray(this.offset, this.offset + length);
    this.offset += length;
    return decodeServiceWireRoutingId(bytes, name, 0xff, fail);
  }

  actorRef(name: string): ServiceActorRef {
    return {
      nodeRid: '',
      actorId: this.text8(`${name}.actorId`),
      generation: this.nonZeroU64(`${name}.generation`)
    };
  }

  optionalActor(): ServiceActorRef | undefined {
    const length = this.u8('sourceActorId.length');
    if (length === 0) return undefined;
    this.need(length, 'sourceActorId');
    const bytes = this.bytes.subarray(this.offset, this.offset + length);
    this.offset += length;
    const actorId = decodeCanonicalServiceWireText(bytes, 'sourceActorId', fail);
    return {
      nodeRid: '',
      actorId,
      generation: this.nonZeroU64('sourceActorGeneration')
    };
  }

  spotRef(): ServiceSpotRef {
    return {
      spotId: this.rid('spotId'),
      generation: this.nonZeroU64('spotGeneration')
    };
  }

  actorFence(): ServiceActorRouteFence {
    const actor = this.actorRef('actor');
    const targetNodeRid = this.rid('targetNodeRid');
    return {
      actor: { ...actor, nodeRid: targetNodeRid },
      targetNodeGeneration: this.nonZeroU64('targetNodeGeneration'),
      authorityOwnerGeneration: this.nonZeroU64('authorityOwnerGeneration'),
      ownerLeaseGeneration: this.nonZeroU64('ownerLeaseGeneration')
    };
  }

  actorAuthorityFence(): ServiceBoundSessionActorAuthority {
    const actor = this.actorRef('actor');
    const targetNodeRid = this.rid('targetNodeRid');
    return {
      actor: { ...actor, nodeRid: targetNodeRid },
      targetNodeGeneration: this.nonZeroU64('targetNodeGeneration'),
      authorityOwnerGeneration: this.nonZeroU64('authorityOwnerGeneration'),
      ownerLeaseGeneration: this.nonZeroU64('ownerLeaseGeneration')
    };
  }

  sessionRelocationOwnerFence(): ServiceSessionRelocationOwnerFence {
    return {
      sessionOwnerNodeRid: this.rid('sessionOwnerNodeRid'),
      sessionOwnerNodeGeneration: this.nonZeroU64('sessionOwnerNodeGeneration'),
      sessionOwnerId: this.text8('sessionOwnerId'),
      sessionOwnerLeaseGeneration: this.nonZeroU64('sessionOwnerLeaseGeneration'),
      sessionRid: this.rid('sessionRid'),
      bindingGeneration: this.nonZeroU64('bindingGeneration')
    };
  }

  spotFence(): ServiceSpotRouteFence {
    const spot = this.spotRef();
    return {
      spot,
      targetNodeRid: this.rid('targetNodeRid'),
      targetNodeGeneration: this.nonZeroU64('targetNodeGeneration'),
      authorityOwnerGeneration: this.nonZeroU64('authorityOwnerGeneration')
    };
  }

  directSpotFence(): ServiceDirectSpotRouteFence {
    return {
      ...this.spotFence(),
      ownerLeaseGeneration: this.nonZeroU64('ownerLeaseGeneration'),
      storeVersion: this.text16('storeVersion')
    };
  }

  messageFollowRoute(name: string): ServiceMessageFollowRoute {
    const kind = this.u8(`${name}.objectKind`);
    const length = this.u16(`${name}.length`);
    const end = this.offset + length;
    if (end > this.bytes.byteLength) fail(`Truncated Message Follow ${name} route.`);
    let value: ServiceMessageFollowRoute;
    if (kind === 1) {
      const actor = this.actorRef(`${name}.actor`);
      value = {
        kind: 'actor',
        actor,
        targetNodeRid: this.rid(`${name}.targetNodeRid`),
        targetNodeGeneration: this.nonZeroU64(`${name}.targetNodeGeneration`),
        authorityOwnerGeneration: this.nonZeroU64(`${name}.authorityOwnerGeneration`),
        ownerLeaseGeneration: this.nonZeroU64(`${name}.ownerLeaseGeneration`)
      };
    } else if (kind === 2) {
      value = {
        kind: 'spot',
        spot: this.spotRef(),
        targetNodeRid: this.rid(`${name}.targetNodeRid`),
        targetNodeGeneration: this.nonZeroU64(`${name}.targetNodeGeneration`),
        authorityOwnerGeneration: this.nonZeroU64(`${name}.authorityOwnerGeneration`),
        ownerLeaseGeneration: this.nonZeroU64(`${name}.ownerLeaseGeneration`)
      };
    } else {
      fail(`Unknown Message Follow ${name} object kind.`);
    }
    if (this.offset !== end) fail(`Invalid Message Follow ${name} route length.`);
    return value;
  }

  end(): void {
    if (this.remaining !== 0) fail('Service wire record has trailing bytes.');
  }

  private sized8(name: string): string {
    const length = this.u8(`${name}.length`);
    if (length === 0) fail(`${name} must not be empty.`);
    this.need(length, name);
    const bytes = this.bytes.subarray(this.offset, this.offset + length);
    this.offset += length;
    return decodeCanonicalServiceWireText(bytes, name, fail);
  }

  private opaque8(name: string): string {
    const length = this.u8(`${name}.length`);
    if (length === 0) fail(`${name} must not be empty.`);
    this.need(length, name);
    const bytes = this.bytes.subarray(this.offset, this.offset + length);
    this.offset += length;
    return decodeServiceWireRoutingId(bytes, name, 0xff, fail);
  }

  private need(count: number, name: string): void {
    if (count < 0 || count > this.remaining) fail(`Truncated ${name}.`);
  }
}

export function decodeServiceWireFrozenRecord(bytes: Uint8Array): ServiceWireFrozenRecord {
  const reader = new FrozenReader(bytes);
  const recordKind = reader.u8('recordKind');
  if (recordKind < 1 || recordKind > 14) fail('Invalid frozen record kind.');
  const sourceKind = reader.u8('sourceKind');
  if (sourceKind < 1 || sourceKind > 4) fail('Invalid frozen source kind.');
  const sourceReader = reader.body16('frozen source');
  const source: ServiceWireRequestSourceFence = {
    nodeRid: sourceReader.rid8('sourceNodeRid'),
    nodeGeneration: sourceReader.nonZeroU64('sourceNodeGeneration'),
    ownerId: sourceReader.text8('sourceOwnerId'),
    leaseGeneration: sourceReader.nonZeroU64('sourceOwnerLeaseGeneration')
  };
  let sourceSpotId: string | undefined;
  let sourceActor: ServiceWireFrozenRecord['sourceActor'];
  let sourceSessionRid: string | undefined;
  let sourceBindingGeneration: bigint | undefined;
  let sourceSessionSequence: bigint | undefined;
  if (sourceKind === 2) {
    sourceSpotId = sourceReader.text8('sourceSpotId');
  } else if (sourceKind === 3 || sourceKind === 4) {
    sourceActor = {
      actorId: sourceReader.text8('sourceActorId'),
      generation: sourceReader.nonZeroU64('sourceActorGeneration')
    };
    if (sourceKind === 4) {
      sourceSessionRid = sourceReader.rid8('sourceSessionRid');
      sourceBindingGeneration = sourceReader.nonZeroU64('sourceBindingGeneration');
      sourceSessionSequence = sourceReader.nonZeroU64('sourceSessionSequence');
    }
  }
  sourceReader.end('frozen source');
  if ([8, 12, 13].includes(recordKind) && sourceKind !== 1) {
    fail('Infrastructure frozen record requires a node source.');
  }
  const hasMetadata = reader.bool8('hasMetadata');
  if (hasMetadata) {
    if (!((recordKind >= 1 && recordKind <= 7)
      || recordKind === 9 || recordKind === 10 || recordKind === 14)) {
      fail('Metadata is forbidden for this frozen record kind.');
    }
    reader.metadata();
  }
  const operationId = {
    high: reader.u64('operationId.high'),
    low: reader.u64('operationId.low')
  };
  const operationKind = reader.u32('operationKind');
  if (operationKind > 15) fail('Invalid frozen operation kind.');
  const replyReader = reader.body16('frozen reply route');
  const requiresReply = operationRequiresReply(operationKind);
  const replyRouteId = requiresReply
    ? replyReader.nonZeroU64('replyRouteId')
    : undefined;
  replyReader.end('frozen reply route');
  const body = reader.frozenBody(recordKind);
  reader.end('frozen record');
  validateFrozenOperationMatrix(recordKind, operationKind, operationId, replyRouteId,
    body.instanceOperationKind);
  return {
    recordKind,
    sourceKind,
    source,
    ...(sourceSpotId === undefined ? {} : { sourceSpotId }),
    ...(sourceActor === undefined ? {} : { sourceActor }),
    ...(sourceSessionRid === undefined ? {} : { sourceSessionRid }),
    ...(sourceBindingGeneration === undefined ? {} : { sourceBindingGeneration }),
    ...(sourceSessionSequence === undefined ? {} : { sourceSessionSequence }),
    hasMetadata,
    operationId,
    operationKind,
    ...(replyRouteId === undefined ? {} : { replyRouteId }),
    ...(body.target === undefined ? {} : { target: body.target }),
    ...(body.applicationPayload === undefined
      ? {}
      : { applicationPayload: body.applicationPayload }),
    canonicalBytes: Buffer.from(bytes)
  };
}

export function encodeServiceWireFrozenRecord(record: ServiceWireFrozenRecord): Buffer {
  if (record.canonicalBytes.byteLength === 0) {
    throw new RangeError('Canonical frozen record bytes must not be empty.');
  }
  const decoded = decodeServiceWireFrozenRecord(record.canonicalBytes);
  if (!sameFrozenRecordSummary(decoded, record)) {
    throw new RangeError('Frozen record summary does not match its canonical bytes.');
  }
  return Buffer.from(record.canonicalBytes);
}

export function encodeServiceWireFrozenActorApplicationRecord(input: {
  readonly source: ServiceWireRequestSourceFence;
  readonly target: {
    readonly actorId: string;
    readonly objectGeneration: bigint;
    readonly nodeRid: string;
    readonly nodeGeneration: bigint;
    readonly authorityOwnerGeneration: bigint;
    readonly ownerLeaseGeneration: bigint;
  };
  readonly operationId: ServiceWireOperationId;
  readonly replyRouteId?: bigint;
  readonly payload: NonNullable<ServiceMaintenanceReplyRelay['payload']>;
}): ServiceWireFrozenRecord {
  const request = input.replyRouteId !== undefined;
  const sourceBody = concat(
    rid(input.source.nodeRid, 'sourceNodeRid'),
    u64(input.source.nodeGeneration),
    text8(input.source.ownerId, 'sourceOwnerId'),
    u64(input.source.leaseGeneration)
  );
  const replyBody = request ? u64(input.replyRouteId!) : Buffer.alloc(0);
  const bytes = concat(
    Buffer.of(request ? 10 : 9),
    Buffer.of(1), u16(sourceBody.byteLength), sourceBody,
    Buffer.of(0),
    wireId(input.operationId, 'operationId'),
    u32(request ? 4 : 0, 'operationKind'),
    u16(replyBody.byteLength), replyBody,
    text8(input.target.actorId, 'targetActorId'),
    u64(input.target.objectGeneration),
    rid(input.target.nodeRid, 'targetNodeRid'),
    u64(input.target.nodeGeneration),
    u64(input.target.authorityOwnerGeneration),
    u64(input.target.ownerLeaseGeneration),
    applicationPayload(input.payload)
  );
  return decodeServiceWireFrozenRecord(bytes);
}

function sameFrozenRecordSummary(left: ServiceWireFrozenRecord, right: ServiceWireFrozenRecord): boolean {
  return left.recordKind === right.recordKind
    && left.sourceKind === right.sourceKind
    && routingIdsEqual(left.source.nodeRid, right.source.nodeRid)
    && left.source.nodeGeneration === right.source.nodeGeneration
    && left.source.ownerId === right.source.ownerId
    && left.source.leaseGeneration === right.source.leaseGeneration
    && left.sourceSpotId === right.sourceSpotId
    && left.sourceActor?.actorId === right.sourceActor?.actorId
    && left.sourceActor?.generation === right.sourceActor?.generation
    && left.sourceSessionRid === right.sourceSessionRid
    && left.sourceBindingGeneration === right.sourceBindingGeneration
    && left.sourceSessionSequence === right.sourceSessionSequence
    && left.hasMetadata === right.hasMetadata
    && left.operationId.high === right.operationId.high
    && left.operationId.low === right.operationId.low
    && left.operationKind === right.operationKind
    && left.replyRouteId === right.replyRouteId
    && left.canonicalBytes.equals(right.canonicalBytes);
}

function validateFrozenOperationMatrix(
  recordKind: number,
  operationKind: number,
  operationId: ServiceWireOperationId,
  replyRouteId: bigint | undefined,
  instanceOperationKind: number | undefined
): void {
  const zero = operationId.high === 0n && operationId.low === 0n;
  let valid = false;
  if ([1, 3, 7, 12, 13].includes(recordKind)) valid = operationKind === 0 && zero;
  else if (recordKind === 2) valid = operationKind === 1 && !zero;
  else if (recordKind === 4) valid = operationKind === 2 && !zero;
  else if (recordKind === 5 || recordKind === 9) valid = operationKind === 0 && !zero;
  else if (recordKind === 6) valid = operationKind === 3 && !zero;
  else if (recordKind === 10) valid = operationKind === 4 && !zero;
  else if (recordKind === 8) {
    valid = operationKind === 0 && zero
      || [6, 7, 8].includes(operationKind) && !zero;
  } else if (recordKind === 11) valid = operationKind >= 1 && operationKind <= 15 && !zero;
  else if (recordKind === 14 && instanceOperationKind !== undefined) {
    valid = instanceOperationKind === 1
      ? operationKind === 0 && zero
      : operationKind === 12 && !zero;
  }
  if (!valid) fail('Frozen record kind, operation kind, and operation ID do not match.');
  if (operationRequiresReply(operationKind) !== (replyRouteId !== undefined)) {
    fail('Frozen reply route does not match the operation kind.');
  }
}

class FrozenReader {
  private offset = 0;
  private readonly bytes: Buffer;

  constructor(bytes: Buffer | Uint8Array) {
    this.bytes = Buffer.isBuffer(bytes)
      ? bytes
      : Buffer.from(bytes.buffer, bytes.byteOffset, bytes.byteLength);
  }

  private get remaining(): number {
    return this.bytes.byteLength - this.offset;
  }

  u8(name: string): number {
    this.need(1, name);
    return this.bytes[this.offset++]!;
  }

  bool8(name: string): boolean {
    const value = this.u8(name);
    if (value !== 0 && value !== 1) fail(`${name} must be bool8.`);
    return value === 1;
  }

  u16(name: string): number {
    this.need(2, name);
    const value = this.bytes.readUInt16BE(this.offset);
    this.offset += 2;
    return value;
  }

  u32(name: string): number {
    this.need(4, name);
    const value = this.bytes.readUInt32BE(this.offset);
    this.offset += 4;
    return value;
  }

  u64(name: string): bigint {
    this.need(8, name);
    const value = this.bytes.readBigUInt64BE(this.offset);
    this.offset += 8;
    return value;
  }

  nonZeroU64(name: string): bigint {
    const value = this.u64(name);
    if (value === 0n) fail(`${name} must be a non-zero u64.`);
    return value;
  }

  text8(name: string): string {
    const length = this.u8(`${name}.length`);
    if (length === 0) fail(`${name} must not be empty.`);
    return this.textBody(length, name);
  }

  rid8(name: string): string {
    const length = this.u8(`${name}.length`);
    if (length === 0) fail(`${name} must not be empty.`);
    this.need(length, name);
    const bytes = this.bytes.subarray(this.offset, this.offset + length);
    this.offset += length;
    return decodeServiceWireRoutingId(bytes, name, 0xff, fail);
  }

  text16(name: string, allowEmpty = false): string {
    const length = this.u16(`${name}.length`);
    if (!allowEmpty && length === 0) fail(`${name} must not be empty.`);
    return this.textBody(length, name);
  }

  body16(name: string): FrozenReader {
    const length = this.u16(`${name}.length`);
    this.need(length, name);
    const body = this.bytes.subarray(this.offset, this.offset + length);
    this.offset += length;
    return new FrozenReader(body);
  }

  applicationPayload(): NonNullable<ServiceMaintenanceReplyRelay['payload']> {
    if (this.u8('applicationPayload.version') !== 1) {
      fail('Application payload envelope version must be one.');
    }
    const body = this.body32('application payload');
    const packetName = body.text8('packetName');
    const contentType = body.text8('contentType');
    const payloadLength = body.u32('payloadLength');
    const bytes = body.take(payloadLength, 'payload');
    body.end('application payload');
    return { packetName, contentType, bytes };
  }

  metadata(): void {
    const start = this.offset;
    if (this.u8('metadata.version') !== 1) fail('Metadata frame version must be one.');
    const count = this.u8('metadata.count');
    const keys = new Set<string>();
    for (let index = 0; index < count; index++) {
      const key = this.text8('metadata.key');
      if (keys.has(key)) fail('Metadata keys must be unique.');
      keys.add(key);
      this.text16('metadata.value', true);
      if (this.offset - start > 1024) fail('Metadata frame exceeds 1024 encoded bytes.');
    }
  }

  frozenBody(recordKind: number): {
    readonly instanceOperationKind?: number;
    readonly target?: ServiceWireFrozenRecord['target'];
    readonly applicationPayload?: NonNullable<ServiceMaintenanceReplyRelay['payload']>;
  } {
    if (recordKind === 1 || recordKind === 2) {
      return { applicationPayload: this.applicationPayload() };
    }
    else if (recordKind === 3 || recordKind === 4) {
      this.text8('channelName');
      return { applicationPayload: this.applicationPayload() };
    } else if (recordKind === 5 || recordKind === 6) {
      return {
        target: this.spotRoute(),
        applicationPayload: this.applicationPayload()
      };
    } else if (recordKind === 7) {
      this.text8('channelName');
      this.text8('topic');
      return { applicationPayload: this.applicationPayload() };
    } else if (recordKind === 8) this.actorControl();
    else if (recordKind === 9 || recordKind === 10) {
      return {
        target: this.actorRoute(),
        applicationPayload: this.applicationPayload()
      };
    } else if (recordKind === 11) {
      const terminalResult = this.u32('terminalResult');
      const failureCode = this.u32('failureCode');
      const hasPayload = this.bool8('hasPayload');
      validateReplyTerminal(terminalResult, failureCode, hasPayload);
      return hasPayload
        ? { applicationPayload: this.applicationPayload() }
        : {};
    } else if (recordKind === 12) this.sendReadyDestination();
    else if (recordKind === 13) {
      validateRelocationPhase(this.u8('phase'));
      const role = this.u8('role');
      if (role < 1 || role > 3) fail('Invalid relocation control role.');
      const relocation = { high: this.u64('relocation.high'), low: this.u64('relocation.low') };
      if (relocation.high === 0n && relocation.low === 0n) fail('Relocation ID must not be zero.');
      this.relocationObject();
      validateReplyTerminal(this.u32('terminalResult'), this.u32('failureCode'), false);
    } else if (recordKind === 14) {
      this.instanceRoute();
      this.nonZeroU64('instanceSourceNodeGeneration');
      const operationKind = this.u8('instanceOperationKind');
      if (operationKind < 1 || operationKind > 2) fail('Invalid Instance activation operation kind.');
      const applicationPayload = this.applicationPayload();
      return { instanceOperationKind: operationKind, applicationPayload };
    }
    return {};
  }

  private spotRoute(): Extract<NonNullable<ServiceWireFrozenRecord['target']>, { kind: 'spot' }> {
    const spot = this.spotIdentity();
    return {
      kind: 'spot',
      ...spot,
      targetNodeRid: this.rid8('targetNodeRid'),
      targetNodeGeneration: this.nonZeroU64('targetNodeGeneration'),
      authorityOwnerGeneration: this.nonZeroU64('expectedAuthorityOwnerGeneration'),
      ownerLeaseGeneration: this.nonZeroU64('expectedOwnerLeaseGeneration')
    };
  }

  private actorRoute(): Extract<NonNullable<ServiceWireFrozenRecord['target']>, { kind: 'actor' }> {
    const actor = this.actorIdentity();
    return {
      kind: 'actor',
      ...actor,
      targetNodeRid: this.rid8('targetNodeRid'),
      targetNodeGeneration: this.nonZeroU64('targetNodeGeneration'),
      authorityOwnerGeneration: this.nonZeroU64('expectedAuthorityOwnerGeneration'),
      ownerLeaseGeneration: this.nonZeroU64('expectedOwnerLeaseGeneration')
    };
  }

  private actorIdentity(): { readonly actorId: string; readonly generation: bigint } {
    return {
      actorId: this.text8('actorId'),
      generation: this.nonZeroU64('actorGeneration')
    };
  }

  private spotIdentity(): { readonly spotId: string; readonly generation: bigint } {
    return {
      spotId: this.text8('spotId'),
      generation: this.nonZeroU64('spotGeneration')
    };
  }

  private membershipSnapshot(): void {
    this.actorIdentity();
    this.spotIdentity();
  }

  private optionalMembershipSnapshot(): void {
    const present = this.bool8('hasMembership');
    const body = this.body16('optionalMembership');
    if (present) body.membershipSnapshot();
    body.end('optional membership');
  }

  private actorControl(): void {
    const lifecycle = this.u8('actorLifecycleKind');
    if (lifecycle < 1 || lifecycle > 5) fail('Invalid Actor lifecycle kind.');
    const body = this.body16('actorControl');
    if (lifecycle === 1 || lifecycle === 4 || lifecycle === 5) body.membershipSnapshot();
    else if (lifecycle === 2) {
      body.optionalMembershipSnapshot();
      body.membershipSnapshot();
    } else {
      body.membershipSnapshot();
      body.membershipSnapshot();
    }
    body.end('Actor control');
  }

  private sendReadyDestination(): void {
    const kind = this.u8('destinationKind');
    if (kind < 1 || kind > 5) fail('Invalid send-ready destination kind.');
    const body = this.body16('send-ready destination');
    if (kind === 1 || kind === 2) body.text8('destination');
    else if (kind === 3) body.spotRoute();
    else {
      body.actorRoute();
      if (kind === 5) body.nonZeroU64('bindingGeneration');
    }
    body.end('send-ready destination');
  }

  private instanceRoute(): void {
    const kind = this.u8('instanceRouteKind');
    if (kind < 1 || kind > 2) fail('Invalid Instance route kind.');
    const body = this.body16('Instance route');
    body.rid8('targetNodeRid');
    body.nonZeroU64('targetNodeGeneration');
    body.text8('targetSpotId');
    if (kind === 1) {
      body.nonZeroU64('objectGeneration');
      body.text8('ownerId');
      body.nonZeroU64('authorityOwnerGeneration');
      body.nonZeroU64('leaseGeneration');
      body.text16('storeVersion');
    } else {
      body.text8('targetMeshName');
      body.text8('stableType');
      body.text8('targetDescriptorVersion');
      body.nonZeroU64('deadlineUnixMs');
    }
    body.end('Instance route');
  }

  private relocationObject(): void {
    const kind = this.u8('relocationObjectKind');
    const body = this.body16('relocation object');
    if (kind === 1) {
      body.text8('actorId');
      body.nonZeroU64('actorGeneration');
      body.nonZeroU64('expectedAuthorityOwnerGeneration');
    } else if (kind === 2) {
      body.text8('spotId');
      body.nonZeroU64('spotGeneration');
      body.nonZeroU64('expectedAuthorityOwnerGeneration');
    } else if (kind === 3) {
      body.text8('stableType');
      body.text8('spotId');
      body.nonZeroU64('objectGeneration');
    } else fail('Invalid relocation object kind.');
    body.end('relocation object');
  }

  private body32(name: string): FrozenReader {
    const length = this.u32(`${name}.length`);
    this.need(length, name);
    const body = this.bytes.subarray(this.offset, this.offset + length);
    this.offset += length;
    return new FrozenReader(body);
  }

  private take(length: number, name: string): Buffer {
    this.need(length, name);
    const value = Buffer.from(this.bytes.subarray(this.offset, this.offset + length));
    this.offset += length;
    return value;
  }

  private textBody(length: number, name: string): string {
    this.need(length, name);
    const bytes = this.bytes.subarray(this.offset, this.offset + length);
    this.offset += length;
    return decodeCanonicalServiceWireText(bytes, name, fail);
  }

  private need(length: number, name: string): void {
    if (length < 0 || length > this.remaining) fail(`Truncated ${name}.`);
  }

  end(name: string): void {
    if (this.remaining !== 0) fail(`${name} has trailing bytes.`);
  }
}
