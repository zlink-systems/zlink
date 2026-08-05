import { OperationRegistry, type PendingOperation } from './operation-registry';

export type ServiceSpotKind = 'entry' | 'user' | 'instance';

export interface ServiceSpotRef {
  readonly spotId: string;
  readonly generation: bigint;
}

export interface ServiceActorRef {
  readonly nodeRid: string;
  readonly actorId: string;
  readonly generation: bigint;
}

export interface ServiceSpotState {
  readonly ref: ServiceSpotRef;
  readonly kind: ServiceSpotKind;
  readonly stableType: string;
  readonly authorityOwnerGeneration: bigint;
  readonly state: 'ready' | 'closing';
}

export interface ServiceActorState {
  readonly ref: ServiceActorRef;
  readonly authorityOwnerGeneration: bigint;
  readonly spot: ServiceSpotRef;
  readonly membershipEpoch: bigint;
}

export interface ServiceSessionBinding {
  readonly actor: ServiceActorRef;
  readonly sessionRid: string;
  readonly sessionOwnerNodeRid: string;
  readonly bindingGeneration: bigint;
  readonly membershipEpoch: bigint;
}

export interface ServiceObjectReservation {
  readonly id: bigint;
  readonly objectKind: 'actor' | 'userSpot' | 'instanceSpot';
  readonly key: string;
  readonly stableType: string;
  readonly generation: bigint;
  readonly attempt: bigint;
}

export type ServiceReservationResult =
  | { readonly kind: 'reserved'; readonly reservation: ServiceObjectReservation }
  | { readonly kind: 'existing'; readonly spot?: ServiceSpotState; readonly actor?: ServiceActorState }
  | { readonly kind: 'typeMismatch' }
  | { readonly kind: 'attemptStale' };

export interface ServiceMembershipTransition {
  readonly actor: ServiceActorState;
  readonly previousSpot: ServiceSpotRef;
  readonly currentSpot: ServiceSpotRef;
  readonly previousMembershipEpoch: bigint;
  readonly currentMembershipEpoch: bigint;
}

export class ServiceStaleGenerationError extends Error {
  constructor(readonly objectKind: 'actor' | 'spot' | 'binding', readonly key: string) {
    super(`${objectKind} '${key}' generation is stale.`);
    this.name = 'ServiceStaleGenerationError';
  }
}

/**
 * Owns the stateful identity and fencing rules that used to be split across
 * Core service tables. Network transport and application materialization stay
 * outside this module.
 */
export class ServiceStatefulRegistry {
  private readonly spots = new Map<string, ServiceSpotState>();
  private readonly spotTypes = new Map<string, { readonly kind: ServiceSpotKind; readonly stableType: string }>();
  private readonly actors = new Map<string, ServiceActorState>();
  private readonly actorTypes = new Map<string, string>();
  private readonly bindings = new Map<string, ServiceSessionBinding>();
  private readonly reservations = new Map<string, ServiceObjectReservation>();
  private readonly spotGenerations = new Map<string, bigint>();
  private readonly actorGenerations = new Map<string, bigint>();
  private readonly reservationAttempts = new Map<string, bigint>();
  private readonly turns = new Map<string, Promise<void>>();
  private nextReservationId = 1n;
  private nextBindingGeneration = 1n;

  constructor(
    readonly nodeRid: string,
    readonly nodeGeneration: bigint
  ) {
    requireText(nodeRid, 'nodeRid');
    requirePositive(nodeGeneration, 'nodeGeneration');
  }

  createEntrySpot(spotId = this.nodeRid): ServiceSpotState {
    const current = this.spots.get(spotId);
    if (current !== undefined) return current;
    return this.restoreSpot(
      { spotId, generation: this.nodeGeneration },
      'entry',
      'entry',
      this.nodeGeneration
    );
  }

  createSpot(
    spotId: string,
    kind: ServiceSpotKind = 'user',
    stableType: string = kind
  ): ServiceSpotState {
    requireText(spotId, 'spotId');
    requireText(stableType, 'stableType');
    const current = this.spots.get(spotId);
    if (current !== undefined) {
      if (current.kind !== kind || current.stableType !== stableType) {
        throw new TypeError(`Spot '${spotId}' is already registered with another kind or type.`);
      }
      return current;
    }
    const assigned = this.spotTypes.get(spotId);
    if (assigned !== undefined && (assigned.kind !== kind || assigned.stableType !== stableType)) {
      throw new TypeError(`Spot '${spotId}' is assigned to another kind or type.`);
    }
    const generation = (this.spotGenerations.get(spotId) ?? 0n) + 1n;
    this.spotGenerations.set(spotId, generation);
    const created: ServiceSpotState = Object.freeze({
      ref: Object.freeze({ spotId, generation }),
      kind,
      stableType,
      authorityOwnerGeneration: generation,
      state: 'ready'
    });
    this.spotTypes.set(spotId, Object.freeze({ kind, stableType }));
    this.spots.set(spotId, created);
    return created;
  }

  spot(spotId: string): ServiceSpotState | undefined {
    return this.spots.get(spotId);
  }

  restoreSpot(
    ref: ServiceSpotRef,
    kind: ServiceSpotKind,
    stableType: string,
    authorityOwnerGeneration: bigint
  ): ServiceSpotState {
    requireText(ref.spotId, 'spotId');
    requirePositive(ref.generation, 'spot.generation');
    requireText(stableType, 'stableType');
    requirePositive(authorityOwnerGeneration, 'authorityOwnerGeneration');
    const current = this.spots.get(ref.spotId);
    if (current !== undefined && current.ref.generation > ref.generation) {
      throw new ServiceStaleGenerationError('spot', ref.spotId);
    }
    const assigned = this.spotTypes.get(ref.spotId);
    if (assigned !== undefined && (assigned.kind !== kind || assigned.stableType !== stableType)) {
      throw new TypeError(`Spot '${ref.spotId}' is assigned to another kind or type.`);
    }
    const restored: ServiceSpotState = Object.freeze({
      ref: Object.freeze({ ...ref }),
      kind,
      stableType,
      authorityOwnerGeneration,
      state: 'ready'
    });
    this.spotGenerations.set(
      ref.spotId,
      max(this.spotGenerations.get(ref.spotId) ?? 0n, ref.generation)
    );
    this.spotTypes.set(ref.spotId, Object.freeze({ kind, stableType }));
    this.spots.set(ref.spotId, restored);
    return restored;
  }

  requireSpot(ref: ServiceSpotRef): ServiceSpotState {
    const current = this.spots.get(ref.spotId);
    if (current === undefined || current.ref.generation !== ref.generation || current.state !== 'ready') {
      throw new ServiceStaleGenerationError('spot', ref.spotId);
    }
    return current;
  }

  closeSpot(ref: ServiceSpotRef): boolean {
    const current = this.requireSpot(ref);
    for (const actor of this.actors.values()) {
      if (
        actor.spot.spotId === ref.spotId
        && actor.spot.generation === ref.generation
      ) {
        return false;
      }
    }
    this.spots.set(ref.spotId, Object.freeze({ ...current, state: 'closing' }));
    this.spots.delete(ref.spotId);
    return true;
  }

  createActor(
    actorId: string,
    stableType = 'actor',
    initialSpot = this.createEntrySpot().ref
  ): ServiceActorState {
    requireText(actorId, 'actorId');
    requireText(stableType, 'stableType');
    this.requireSpot(initialSpot);
    const current = this.actors.get(actorId);
    if (current !== undefined) {
      if (this.actorTypes.get(actorId) !== stableType) {
        throw new TypeError(`Actor '${actorId}' is already registered with another type.`);
      }
      return current;
    }
    const assignedType = this.actorTypes.get(actorId);
    if (assignedType !== undefined && assignedType !== stableType) {
      throw new TypeError(`Actor '${actorId}' is assigned to another type.`);
    }
    const generation = (this.actorGenerations.get(actorId) ?? 0n) + 1n;
    this.actorGenerations.set(actorId, generation);
    const created: ServiceActorState = Object.freeze({
      ref: Object.freeze({
        nodeRid: this.nodeRid,
        actorId,
        generation
      }),
      authorityOwnerGeneration: generation,
      spot: Object.freeze({ ...initialSpot }),
      membershipEpoch: 1n
    });
    this.actorTypes.set(actorId, stableType);
    this.actors.set(actorId, created);
    return created;
  }

  restoreActor(
    actor: ServiceActorRef,
    stableType: string,
    spot: ServiceSpotRef,
    membershipEpoch: bigint,
    authorityOwnerGeneration = actor.generation
  ): ServiceActorState {
    this.requireSpot(spot);
    requirePositive(actor.generation, 'actor.generation');
    requirePositive(membershipEpoch, 'membershipEpoch');
    requirePositive(authorityOwnerGeneration, 'authorityOwnerGeneration');
    const current = this.actors.get(actor.actorId);
    if (current !== undefined && current.ref.generation > actor.generation) {
      throw new ServiceStaleGenerationError('actor', actor.actorId);
    }
    const assignedType = this.actorTypes.get(actor.actorId);
    if (assignedType !== undefined && assignedType !== stableType) {
      throw new TypeError(`Actor '${actor.actorId}' is assigned to another type.`);
    }
    const restored: ServiceActorState = Object.freeze({
      ref: Object.freeze({ ...actor, nodeRid: this.nodeRid }),
      authorityOwnerGeneration,
      spot: Object.freeze({ ...spot }),
      membershipEpoch
    });
    this.actorGenerations.set(
      actor.actorId,
      max(this.actorGenerations.get(actor.actorId) ?? 0n, actor.generation)
    );
    this.actorTypes.set(actor.actorId, stableType);
    this.actors.set(actor.actorId, restored);
    return restored;
  }

  actor(actorId: string): ServiceActorState | undefined {
    return this.actors.get(actorId);
  }

  requireActor(ref: ServiceActorRef): ServiceActorState {
    const current = this.actors.get(ref.actorId);
    if (
      current === undefined
      || current.ref.generation !== ref.generation
      || current.ref.nodeRid !== ref.nodeRid
    ) {
      throw new ServiceStaleGenerationError('actor', ref.actorId);
    }
    return current;
  }

  joinActor(actor: ServiceActorRef, target: ServiceSpotRef): ServiceMembershipTransition {
    const current = this.requireActor(actor);
    this.requireSpot(target);
    const next: ServiceActorState = Object.freeze({
      ...current,
      spot: Object.freeze({ ...target }),
      membershipEpoch: current.membershipEpoch + 1n
    });
    this.actors.set(actor.actorId, next);
    this.updateBindingMembership(next);
    return {
      actor: next,
      previousSpot: current.spot,
      currentSpot: next.spot,
      previousMembershipEpoch: current.membershipEpoch,
      currentMembershipEpoch: next.membershipEpoch
    };
  }

  leaveActor(actor: ServiceActorRef, expectedMembershipEpoch: bigint): ServiceMembershipTransition {
    const entry = this.createEntrySpot().ref;
    const current = this.requireActor(actor);
    if (current.membershipEpoch !== expectedMembershipEpoch) {
      throw new ServiceStaleGenerationError('actor', actor.actorId);
    }
    return this.joinActor(actor, entry);
  }

  destroyActor(actor: ServiceActorRef): ServiceActorState {
    const current = this.requireActor(actor);
    this.actors.delete(actor.actorId);
    this.bindings.delete(actorKey(actor));
    return current;
  }

  bindSession(
    actor: ServiceActorRef,
    sessionRid: string,
    sessionOwnerNodeRid: string
  ): ServiceSessionBinding {
    const current = this.requireActor(actor);
    requireText(sessionRid, 'sessionRid');
    requireText(sessionOwnerNodeRid, 'sessionOwnerNodeRid');
    const binding: ServiceSessionBinding = Object.freeze({
      actor: Object.freeze({ ...current.ref }),
      sessionRid,
      sessionOwnerNodeRid,
      bindingGeneration: this.nextBindingGeneration++,
      membershipEpoch: current.membershipEpoch
    });
    this.bindings.set(actorKey(actor), binding);
    return binding;
  }

  installSessionBinding(binding: ServiceSessionBinding): void {
    this.requireActor(binding.actor);
    requirePositive(binding.bindingGeneration, 'bindingGeneration');
    const current = this.bindings.get(actorKey(binding.actor));
    if (
      current !== undefined
      && current.sessionOwnerNodeRid === binding.sessionOwnerNodeRid
      && current.sessionRid === binding.sessionRid
      && current.bindingGeneration >= binding.bindingGeneration
    ) {
      throw new ServiceStaleGenerationError('binding', binding.actor.actorId);
    }
    this.nextBindingGeneration = max(this.nextBindingGeneration, binding.bindingGeneration + 1n);
    this.bindings.set(actorKey(binding.actor), Object.freeze({ ...binding }));
  }

  binding(actor: ServiceActorRef): ServiceSessionBinding | undefined {
    const binding = this.bindings.get(actorKey(actor));
    return binding?.actor.generation === actor.generation ? binding : undefined;
  }

  unbindSession(
    actor: ServiceActorRef,
    expectedBindingGeneration: bigint,
    expectedSessionRid?: string,
    expectedSessionOwnerNodeRid?: string
  ): boolean {
    const key = actorKey(actor);
    const binding = this.bindings.get(key);
    if (binding === undefined) return false;
    if (
      binding.bindingGeneration !== expectedBindingGeneration
      || expectedSessionRid !== undefined && binding.sessionRid !== expectedSessionRid
      || expectedSessionOwnerNodeRid !== undefined
        && binding.sessionOwnerNodeRid !== expectedSessionOwnerNodeRid
    ) {
      throw new ServiceStaleGenerationError('binding', actor.actorId);
    }
    this.bindings.delete(key);
    return true;
  }

  validateBoundSession(actor: ServiceActorRef, expectedBindingGeneration: bigint): ServiceSessionBinding {
    const binding = this.binding(actor);
    if (binding === undefined || binding.bindingGeneration !== expectedBindingGeneration) {
      throw new ServiceStaleGenerationError('binding', actor.actorId);
    }
    return binding;
  }

  reserve(
    objectKind: ServiceObjectReservation['objectKind'],
    key: string,
    stableType: string,
    attempt: bigint
  ): ServiceReservationResult {
    requireText(key, 'key');
    requireText(stableType, 'stableType');
    requirePositive(attempt, 'attempt');
    const reservationKey = `${objectKind}:${key}`;
    const currentAttempt = this.reservationAttempts.get(reservationKey) ?? 0n;
    if (attempt < currentAttempt) return { kind: 'attemptStale' };

    if (objectKind === 'actor') {
      const actor = this.actors.get(key);
      if (actor !== undefined) {
        return this.actorTypes.get(key) === stableType
          ? { kind: 'existing', actor }
          : { kind: 'typeMismatch' };
      }
      const assignedType = this.actorTypes.get(key);
      if (assignedType !== undefined && assignedType !== stableType) {
        return { kind: 'typeMismatch' };
      }
    } else {
      const spot = this.spots.get(key);
      const expectedKind = objectKind === 'userSpot' ? 'user' : 'instance';
      if (spot !== undefined) {
        return spot.kind === expectedKind && spot.stableType === stableType
          ? { kind: 'existing', spot }
          : { kind: 'typeMismatch' };
      }
      const assigned = this.spotTypes.get(key);
      if (
        assigned !== undefined
        && (assigned.kind !== expectedKind || assigned.stableType !== stableType)
      ) {
        return { kind: 'typeMismatch' };
      }
    }

    const active = this.reservations.get(reservationKey);
    if (active !== undefined) {
      if (active.stableType !== stableType) return { kind: 'typeMismatch' };
      if (attempt < active.attempt) return { kind: 'attemptStale' };
      if (attempt === active.attempt) return { kind: 'reserved', reservation: active };
    }
    const generation = objectKind === 'actor'
      ? (this.actorGenerations.get(key) ?? 0n) + 1n
      : (this.spotGenerations.get(key) ?? 0n) + 1n;
    const reservation = Object.freeze({
      id: this.nextReservationId++,
      objectKind,
      key,
      stableType,
      generation,
      attempt
    });
    this.reservationAttempts.set(reservationKey, attempt);
    this.reservations.set(reservationKey, reservation);
    return { kind: 'reserved', reservation };
  }

  commitReservation(reservation: ServiceObjectReservation): ServiceSpotState | ServiceActorState {
    const key = `${reservation.objectKind}:${reservation.key}`;
    if (this.reservations.get(key)?.id !== reservation.id) {
      throw new ServiceStaleGenerationError(
        reservation.objectKind === 'actor' ? 'actor' : 'spot',
        reservation.key
      );
    }
    this.reservations.delete(key);
    if (reservation.objectKind === 'actor') {
      return this.createActor(reservation.key, reservation.stableType);
    }
    const spotKind: ServiceSpotKind =
      reservation.objectKind === 'instanceSpot' ? 'instance' : 'user';
    return this.createSpot(
      reservation.key,
      spotKind,
      reservation.stableType
    );
  }

  abortReservation(reservation: ServiceObjectReservation): boolean {
    const key = `${reservation.objectKind}:${reservation.key}`;
    if (this.reservations.get(key)?.id !== reservation.id) return false;
    this.reservations.delete(key);
    return true;
  }

  async runTurn<T>(owner: string, work: () => T | Promise<T>): Promise<T> {
    requireText(owner, 'owner');
    const previous = this.turns.get(owner) ?? Promise.resolve();
    let release!: () => void;
    const barrier = new Promise<void>(resolve => {
      release = resolve;
    });
    const tail = previous.then(() => barrier);
    this.turns.set(owner, tail);
    await previous;
    try {
      return await work();
    } finally {
      release();
      if (this.turns.get(owner) === tail) this.turns.delete(owner);
    }
  }

  private updateBindingMembership(actor: ServiceActorState): void {
    const key = actorKey(actor.ref);
    const binding = this.bindings.get(key);
    if (binding === undefined) return;
    this.bindings.set(key, Object.freeze({
      ...binding,
      actor: actor.ref,
      membershipEpoch: actor.membershipEpoch
    }));
  }
}

export class ServiceTerminalOperationRegistry<T> {
  private readonly operations: OperationRegistry<T>;

  constructor(operations = new OperationRegistry<T>()) {
    this.operations = operations;
  }

  reserve(timeoutMs: number): PendingOperation<T> {
    return this.operations.reserve(timeoutMs);
  }

  reply(id: bigint, value: T): boolean {
    return this.operations.complete(id, value);
  }

  fail(id: bigint, error: unknown): boolean {
    return this.operations.fail(id, error);
  }

  cancel(id: bigint): boolean {
    return this.operations.cancel(id);
  }

  isPending(id: bigint): boolean {
    return this.operations.isPending(id);
  }

  close(): void {
    this.operations.close('Stateful runtime closed.');
  }
}

function actorKey(actor: ServiceActorRef): string {
  return `${actor.actorId}\0${actor.generation}`;
}

function requireText(value: string, name: string): void {
  const bytes = Buffer.byteLength(value);
  if (bytes < 1 || bytes > 255 || value.includes('\0')) {
    throw new RangeError(`${name} must be 1..255 UTF-8 bytes without NUL.`);
  }
}

function requirePositive(value: bigint, name: string): void {
  if (value < 1n) throw new RangeError(`${name} must be positive.`);
}

function max(left: bigint, right: bigint): bigint {
  return left > right ? left : right;
}
