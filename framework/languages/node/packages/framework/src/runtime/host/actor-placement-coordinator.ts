import { ZLinkFrameworkInternalErrorKind, createInternalFrameworkException  } from '../framework-errors-internal';
import { createHash } from 'node:crypto';
import { RequestResult } from '../backend/runtime-values';
import type {
  ActorRef,
  RoutingId,
  ZLinkActorCreateResult
} from '../../contracts';
import type {
  ZLinkAuthoritySnapshot,
  ZLinkLocationOwnerToken,
  ZLinkObjectReserveResult
} from '../../contracts/Locations';
import type {
  ZLinkAuthorityStore,
  ZLinkObjectCreationStore
} from '../locations/internal-store-contracts';
import { encodeAuthorityKey } from '../locations/authority-key-codec';
import type {
  ServiceActorCreateRecord,
  ServiceUserSpotReservationFence
} from '../foundation/service-stateful-wire-codec';
import type {
  ServiceUserSpotOperationResult
} from '../foundation/service-stateful-runtime';
import {
  decodeActorAuthorityIdentity,
  encodeActorAuthorityIdentity
} from '../actors/actor-authority-publication';
import {
  decodeLocationCreationContent,
  encodeLocationCreationContent
} from './user-spot-creation-coordinator';

export interface ZLinkActorPlacementTarget {
  readonly meshName: string;
  readonly nodeRid: RoutingId;
  readonly nodeGeneration: bigint;
  readonly owner: ZLinkLocationOwnerToken;
  readonly isLocal: boolean;
}

export interface ZLinkActorPlacementCoordinatorOptions {
  readonly store: ZLinkObjectCreationStore & ZLinkAuthorityStore;
  readonly target: (
    meshName: string | undefined,
    stableType: string,
    signal?: AbortSignal,
    excludedNodeRids?: ReadonlySet<string>
  ) => Promise<ZLinkActorPlacementTarget | undefined>;
  readonly remoteCreate: (
    meshName: string,
    targetNodeRid: string,
    request: Omit<ServiceActorCreateRecord, 'kind' | 'correlation' | 'operation'>,
    timeoutMs: number
  ) => Promise<ServiceUserSpotOperationResult>;
  readonly decodeRemoteReply?: (payload: Uint8Array) => unknown;
}

export class ZLinkActorPlacementCoordinator {
  constructor(private readonly options: ZLinkActorPlacementCoordinatorOptions) {}

  async create(
    actorId: string,
    stableType: string,
    createOnly: boolean,
    meshName: string | undefined,
    requestPayload: Uint8Array,
    timeoutMs: number,
    signal?: AbortSignal
  ): Promise<ZLinkActorCreateResult> {
    const deadline = createDeadline(timeoutMs, signal);
    const deadlineUnixMs = Date.now() + timeoutMs;
    const contentReference = encodeLocationCreationContent(requestPayload);
    const requestSha256 = createHash('sha256').update(requestPayload).digest();
    const excluded = new Set<string>();
    try {
      for (;;) {
        const target = await this.options.target(meshName, stableType, deadline.signal, excluded);
        if (target === undefined) {
          throw createInternalFrameworkException(
            ZLinkFrameworkInternalErrorKind.PlacementCapacityExhausted,
            `No eligible Actor placement target is ready for '${stableType}'.`,
            true
          );
        }
        const creatingPayload = encodeActorAuthorityIdentity({
          actorType: stableType,
          actor: {
            actorId,
            objectGeneration: 1n,
            meshName: target.meshName,
            nodeRid: target.nodeRid
          },
          meshName: target.meshName,
          ownerNodeGeneration: target.nodeGeneration,
          owner: target.owner
        });
        const reserved = await this.options.store.reserve({
          key: { kind: 'actor', globalId: actorId },
          intent: {
            stableType,
            requestContentReference: contentReference,
            requestSha256,
            requestEncodedSize: BigInt(requestPayload.byteLength)
          },
          target: reservationTarget(target),
          creatingPayload,
          capacity: { actors: 1, spots: 0 }
        }, deadline.signal);
        const existing = existingActor(reserved, actorId, stableType);
        if (existing !== undefined) {
          if (createOnly) {
            throw createInternalFrameworkException(
              ZLinkFrameworkInternalErrorKind.ActorAlreadyExists,
              `Actor '${actorId}' already exists.`
            );
          }
          return { status: 'existing', actor: existing };
        }
        if (reserved.kind === 'typeMismatch') {
          throw createInternalFrameworkException(
            ZLinkFrameworkInternalErrorKind.ActorTypeMismatch,
            `Actor '${actorId}' is registered with another stable type.`
          );
        }
        if (reserved.kind === 'placementCapacityExhausted') {
          excluded.add(String(target.nodeRid));
          continue;
        }
        if (reserved.kind === 'alreadyExists' || reserved.kind === 'conflict') {
          await waitForAuthorityChange(deadline.signal);
          continue;
        }
        if (reserved.kind !== 'reserved') {
          throw createInternalFrameworkException(
            ZLinkFrameworkInternalErrorKind.ActorCreateFailed,
            `Actor '${actorId}' reservation failed with '${reserved.kind}'.`,
            true
          );
        }
        const remote = await this.options.remoteCreate(
          target.meshName,
          String(target.nodeRid),
          actorCreateRecord(
            actorId,
            stableType,
            reserved.creating,
            target,
            deadlineUnixMs
          ),
          Math.max(1, deadlineUnixMs - Date.now())
        );
        if (
          remote.terminalResult !== RequestResult.Ok
          || remote.tail?.kind !== 'actorCreate'
        ) {
          throw createInternalFrameworkException(
            ZLinkFrameworkInternalErrorKind.ActorCreateFailed,
            `Remote Actor '${actorId}' creation failed.`,
            remote.terminalResult !== RequestResult.InvalidState
          );
        }
        if (remote.tail.createResult === 'rejected') {
          return {
            status: 'rejected',
            ...(remote.payload === undefined
              ? {}
              : {
                  reply: this.options.decodeRemoteReply?.(remote.payload.payload)
                    ?? Buffer.from(remote.payload.payload)
                })
          };
        }
        const actor = remote.tail.actor;
        if (actor === undefined) {
          throw createInternalFrameworkException(
            ZLinkFrameworkInternalErrorKind.ActorCreateFailed,
            'Remote Actor create terminal omitted ActorRef.'
          );
        }
        const ref = withActorAuthorityFence({
          actorId: actor.actorId,
          objectGeneration: actor.generation,
          meshName: target.meshName,
          nodeRid: actor.nodeRid as RoutingId
        }, reserved.creating);
        return {
          status: remote.tail.createResult,
          actor: ref,
          ...(remote.payload === undefined
            ? {}
            : {
                reply: this.options.decodeRemoteReply?.(remote.payload.payload)
                  ?? Buffer.from(remote.payload.payload)
              })
        };
      }
    } finally {
      deadline.close();
    }
  }

  async handleRemoteCreate(
    record: ServiceActorCreateRecord,
    materialize: (
      requestPayload: Uint8Array,
      authority: ZLinkAuthoritySnapshot,
      signal: AbortSignal
    ) => Promise<{
      readonly result: 'created' | 'rejected';
      readonly actor?: ActorRef;
      readonly reply?: Uint8Array;
      readonly onPublished?: () => void;
    }>,
    signal: AbortSignal
  ): Promise<ServiceUserSpotOperationResult> {
    const key = { kind: 'actor' as const, globalId: record.actorId };
    const current = await this.options.store.readAuthority(
      encodeAuthorityKey('actor', record.actorId),
      signal
    );
    requireExactReservation(current, record);
    if (current.kind !== 'snapshot') {
      throw createInternalFrameworkException(
        ZLinkFrameworkInternalErrorKind.ActorCreateFailed,
        `Actor '${record.actorId}' reservation is missing.`
      );
    }
    if (current.allocation.state === 'active') {
      const identity = decodeActorAuthorityIdentity(current.payload);
      if (identity === undefined) {
        throw createInternalFrameworkException(
          ZLinkFrameworkInternalErrorKind.ActorCreateFailed,
          `Actor '${record.actorId}' Ready authority is invalid.`
        );
      }
      return actorResult('existing', identity.actor);
    }
    const pending = current.pendingCreation!;
    const requestPayload = decodeLocationCreationContent(
      pending.requestContentReference,
      pending.requestSha256,
      pending.requestEncodedSize
    );
    const local = await materialize(requestPayload, current, signal);
    const terminal = encodeActorTerminal(
      record.correlation,
      local.result,
      local.actor
    );
    const completion = await this.options.store.completeCreation({
      key,
      reservationId: record.reservation.reservationId,
      expectedStoreVersion: current.storeVersion.value,
      target: {
        meshName: current.allocation.descriptor.meshName,
        nodeRid: current.allocation.descriptor.rid,
        nodeLifecycleGeneration: current.allocation.descriptorLifecycleGeneration,
        owner: {
          ownerId: current.ownerId,
          leaseGeneration: current.ownerLeaseGeneration
        }
      },
      completion: local.result === 'created'
        ? {
            kind: 'created',
            readyPayload: encodeActorAuthorityIdentity({
              actorType: record.stableType,
              actor: local.actor!,
              meshName: current.allocation.descriptor.meshName,
              ownerNodeGeneration: current.allocation.descriptorLifecycleGeneration,
              owner: {
                ownerId: current.ownerId,
                leaseGeneration: current.ownerLeaseGeneration
              }
            }),
            terminal: terminalPublication(record, terminal)
          }
        : {
            kind: 'rejected',
            terminal: terminalPublication(record, terminal)
          }
    }, signal);
    if (
      local.result === 'created'
        ? completion.kind !== 'created' && completion.kind !== 'alreadyCompleted'
        : completion.kind !== 'rejected' && completion.kind !== 'alreadyCompleted'
    ) {
      throw createInternalFrameworkException(
        ZLinkFrameworkInternalErrorKind.ActorCreateFailed,
        `Actor '${record.actorId}' creation completion lost its reservation.`
      );
    }
    if (local.result === 'created') local.onPublished?.();
    return actorResult(local.result, local.actor, local.reply);
  }
}

function existingActor(
  reserved: ZLinkObjectReserveResult,
  actorId: string,
  stableType: string
): ActorRef | undefined {
  const snapshot = reserved.kind === 'alreadyExists'
    ? reserved.current
    : reserved.kind === 'conflict' && reserved.current.kind === 'snapshot'
      ? reserved.current
      : undefined;
  if (
    snapshot === undefined
    || snapshot.allocation.state !== 'active'
    || snapshot.allocation.objectKind !== 'actor'
    || snapshot.allocation.stableType !== stableType
  ) {
    return undefined;
  }
  const identity = decodeActorAuthorityIdentity(snapshot.payload);
  return identity?.actor.actorId === actorId
    ? withActorAuthorityFence(identity.actor, snapshot)
    : undefined;
}

function withActorAuthorityFence(
  actor: ActorRef,
  authority: ZLinkAuthoritySnapshot
): ActorRef {
  Object.defineProperties(actor, {
    ownershipGeneration: {
      configurable: false,
      enumerable: false,
      value: authority.authorityOwnerGeneration
    },
    ownerLeaseGeneration: {
      configurable: false,
      enumerable: false,
      value: authority.ownerLeaseGeneration
    }
  });
  return actor;
}

function actorCreateRecord(
  actorId: string,
  stableType: string,
  snapshot: ZLinkAuthoritySnapshot,
  target: ZLinkActorPlacementTarget,
  deadlineUnixMs: number
): Omit<ServiceActorCreateRecord, 'kind' | 'correlation' | 'operation'> {
  const pending = snapshot.pendingCreation;
  if (pending === undefined) throw new Error('Actor reservation omitted Pending creation.');
  return {
    sourceNodeRid: '',
    sourceNodeGeneration: 1n,
    actorId,
    stableType,
    reservation: reservationFence(snapshot, target, pending.reservationId),
    deadlineUnixMs: BigInt(deadlineUnixMs)
  };
}

function reservationFence(
  snapshot: ZLinkAuthoritySnapshot,
  target: ZLinkActorPlacementTarget,
  reservationId: string
): ServiceUserSpotReservationFence {
  return {
    reservationId,
    expectedStoreVersion: snapshot.storeVersion.value,
    objectGeneration: snapshot.objectGeneration,
    authorityOwnerGeneration: snapshot.authorityOwnerGeneration,
    targetNodeRid: String(target.nodeRid),
    targetNodeGeneration: target.nodeGeneration,
    targetOwnerId: target.owner.ownerId,
    targetOwnerLeaseGeneration: target.owner.leaseGeneration,
    pendingCapacityDelta: 1
  };
}

function reservationTarget(target: ZLinkActorPlacementTarget) {
  return {
    meshName: target.meshName,
    nodeRid: target.nodeRid,
    nodeLifecycleGeneration: target.nodeGeneration,
    owner: target.owner
  };
}

function requireExactReservation(
  current: Awaited<ReturnType<ZLinkAuthorityStore['readAuthority']>>,
  record: ServiceActorCreateRecord
): void {
  const pending = current.kind === 'snapshot' ? current.pendingCreation : undefined;
  if (
    current.kind !== 'snapshot'
    || current.allocation.objectKind !== 'actor'
    || current.allocation.stableType !== record.stableType
    || current.objectGeneration !== record.reservation.objectGeneration
    || current.authorityOwnerGeneration !== record.reservation.authorityOwnerGeneration
    || String(current.allocation.descriptor.rid) !== record.reservation.targetNodeRid
    || current.allocation.descriptorLifecycleGeneration !== record.reservation.targetNodeGeneration
    || current.ownerId !== record.reservation.targetOwnerId
    || current.ownerLeaseGeneration !== record.reservation.targetOwnerLeaseGeneration
    || (
      current.allocation.state === 'reserved'
      && (
        current.storeVersion.value !== record.reservation.expectedStoreVersion
        || pending?.reservationId !== record.reservation.reservationId
      )
    )
  ) {
    throw createInternalFrameworkException(
      ZLinkFrameworkInternalErrorKind.ActorCreateFailed,
      `Actor '${record.actorId}' reservation fence is stale.`,
      true
    );
  }
}

function actorResult(
  createResult: 'existing' | 'created' | 'rejected',
  actor?: ActorRef,
  reply?: Uint8Array
): ServiceUserSpotOperationResult {
  return {
    terminalResult: RequestResult.Ok,
    failureCode: 0,
    tail: {
      kind: 'actorCreate',
      createResult,
      ...(actor === undefined
        ? {}
        : {
            actor: {
              actorId: actor.actorId,
              generation: actor.objectGeneration,
              nodeRid: String(actor.nodeRid)
            }
          })
    },
    ...(reply === undefined
      ? {}
      : {
          payload: {
            packetName: 'ZLinkFrameworkActorCreateReply',
            contentType: 'application/octet-stream',
            payload: Buffer.from(reply)
          }
        })
  };
}

function encodeActorTerminal(
  correlation: bigint,
  result: 'created' | 'rejected',
  actor?: ActorRef
): Buffer {
  return Buffer.from(JSON.stringify({
    correlation: correlation.toString(),
    result,
    actor: actor === undefined
      ? undefined
      : {
          actorId: actor.actorId,
          generation: actor.objectGeneration.toString(),
          meshName: actor.meshName,
          nodeRid: String(actor.nodeRid)
        }
  }));
}

function terminalPublication(record: ServiceActorCreateRecord, terminalEnvelope: Uint8Array) {
  return {
    operation: {
      sourceNodeRid: record.sourceNodeRid as RoutingId,
      sourceNodeGeneration: record.sourceNodeGeneration,
      operationId: record.operation
    },
    terminalEnvelope,
    terminalEnvelopeSha256: createHash('sha256').update(terminalEnvelope).digest(),
    operationDeadline: new Date(Number(record.deadlineUnixMs))
  };
}

function createDeadline(timeoutMs: number, parent?: AbortSignal) {
  const controller = new AbortController();
  const abort = () => controller.abort(parent?.reason);
  parent?.addEventListener('abort', abort, { once: true });
  const timer = setTimeout(
    () => controller.abort(new Error('Actor creation deadline exceeded.')),
    Math.max(1, timeoutMs)
  );
  return {
    signal: controller.signal,
    close: () => {
      clearTimeout(timer);
      parent?.removeEventListener('abort', abort);
    }
  };
}

async function waitForAuthorityChange(signal: AbortSignal): Promise<void> {
  signal.throwIfAborted();
  await new Promise<void>((resolve, reject) => {
    const timer = setTimeout(finish, 10);
    const abort = () => finish(signal.reason ?? new Error('Actor creation was aborted.'));
    function finish(error?: unknown) {
      clearTimeout(timer);
      signal.removeEventListener('abort', abort);
      error === undefined ? resolve() : reject(error);
    }
    signal.addEventListener('abort', abort, { once: true });
  });
}
