import type { ZLinkAuthorityKey, ZLinkAuthoritySnapshot } from '../../contracts/Locations';
import type {
  ServiceRelocationRestoreOwner,
  ServiceRelocationSourceCompletion,
  ServiceRelocationStaging
} from './service-relocation-coordinator';
import type {
  ServiceRelocationEnvelope,
  ServiceRelocationMembership,
  ServiceRelocationParticipant,
  ServiceRelocationParticipantProgress,
  ServiceRelocationQueuedMessage,
  ServiceRelocationTimer
} from './service-relocation-runtime';

export interface ServiceRelocationSealedWork {
  readonly acceptedJournal: Uint8Array;
  readonly queuedMessages: readonly ServiceRelocationQueuedMessage[];
  readonly timers: readonly ServiceRelocationTimer[];
}

export interface ServiceRelocationCaptureUnit {
  readonly authorityKey: string;
  readonly objectKind: 'actor' | 'user_spot' | 'instance_spot';
  readonly stableType: string;
  readonly objectGeneration: bigint;
  readonly authorityOwnerGeneration: bigint;
  seal(signal?: AbortSignal): Promise<ServiceRelocationSealedWork>;
  captureApplicationState(signal?: AbortSignal): Promise<Uint8Array>;
  commitSeal(): Promise<void> | void;
  abortSeal(): Promise<void> | void;
}

export class ServiceCapturedObjectRelocation {
  private terminal: 'open' | 'committed' | 'aborted' = 'open';

  constructor(
    readonly envelope: ServiceRelocationEnvelope,
    private readonly units: readonly ServiceRelocationCaptureUnit[]
  ) {}

  async commitSource(): Promise<void> {
    if (this.terminal === 'committed') return;
    if (this.terminal === 'aborted') {
      throw new Error('Relocation source capture was already aborted.');
    }
    for (const unit of this.units) await unit.commitSeal();
    this.terminal = 'committed';
  }

  async abortSource(): Promise<void> {
    if (this.terminal === 'aborted') return;
    if (this.terminal === 'committed') {
      throw new Error('Committed relocation source capture cannot be aborted.');
    }
    for (const unit of [...this.units].reverse()) await unit.abortSeal();
    this.terminal = 'aborted';
  }
}

/** Captures the two valid relocation units: User Spot aggregate or one Actor. */
export class ServiceRelocationObjectCaptureOwner {
  constructor(private readonly maxConcurrentCallbacks = 8) {
    requireCallbackConcurrency(maxConcurrentCallbacks);
  }

  async captureUserSpotAggregate(
    aggregateId: string,
    aggregateGeneration: bigint,
    spot: ServiceRelocationCaptureUnit,
    actors: readonly ServiceRelocationCaptureUnit[],
    memberships: readonly ServiceRelocationMembership[],
    signal?: AbortSignal
  ): Promise<ServiceCapturedObjectRelocation> {
    if (spot.objectKind !== 'user_spot' || actors.some(actor => actor.objectKind !== 'actor')) {
      throw new TypeError('User Spot relocation requires one Spot and only Actor participants.');
    }
    if (actors.length !== memberships.length) {
      throw new TypeError('User Spot relocation membership must cover every Actor exactly once.');
    }
    return await this.capture(
      aggregateId,
      aggregateGeneration,
      [spot, ...actors],
      memberships,
      signal
    );
  }

  async captureStandaloneActor(
    aggregateId: string,
    aggregateGeneration: bigint,
    actor: ServiceRelocationCaptureUnit,
    membership: ServiceRelocationMembership,
    signal?: AbortSignal
  ): Promise<ServiceCapturedObjectRelocation> {
    if (actor.objectKind !== 'actor' || membership.actorKey !== actor.authorityKey) {
      throw new TypeError('Standalone Actor relocation has an invalid membership identity.');
    }
    return await this.capture(
      aggregateId,
      aggregateGeneration,
      [actor],
      [membership],
      signal
    );
  }

  async captureInstanceSpotAggregate(
    aggregateId: string,
    aggregateGeneration: bigint,
    spot: ServiceRelocationCaptureUnit,
    actors: readonly ServiceRelocationCaptureUnit[],
    memberships: readonly ServiceRelocationMembership[],
    signal?: AbortSignal
  ): Promise<ServiceCapturedObjectRelocation> {
    if (spot.objectKind !== 'instance_spot'
      || actors.some(actor => actor.objectKind !== 'actor')) {
      throw new TypeError(
        'Instance Spot relocation requires one Spot and only Actor participants.'
      );
    }
    if (actors.length !== memberships.length) {
      throw new TypeError(
        'Instance Spot relocation membership must cover every Actor exactly once.'
      );
    }
    return await this.capture(
      aggregateId,
      aggregateGeneration,
      [spot, ...actors],
      memberships,
      signal
    );
  }

  private async capture(
    aggregateId: string,
    aggregateGeneration: bigint,
    units: readonly ServiceRelocationCaptureUnit[],
    memberships: readonly ServiceRelocationMembership[],
    signal?: AbortSignal
  ): Promise<ServiceCapturedObjectRelocation> {
    const captured: Array<{
      readonly unit: ServiceRelocationCaptureUnit;
      readonly work: ServiceRelocationSealedWork;
    }> = [];
    try {
      for (const unit of units) {
        signal?.throwIfAborted();
        const work = await unit.seal(signal);
        captured.push({ unit, work });
      }
      const participants = await mapConcurrentOrdered(
        captured,
        this.maxConcurrentCallbacks,
        async ({ unit, work }) => ({
          key: unit.authorityKey,
          objectKind: unit.objectKind,
          stableType: unit.stableType,
          objectGeneration: unit.objectGeneration,
          authorityOwnerGeneration: unit.authorityOwnerGeneration,
          applicationState: Buffer.from(await unit.captureApplicationState(signal)),
          acceptedJournal: Buffer.from(work.acceptedJournal),
          replayCursor: 0n,
          terminalReplies: Buffer.alloc(0),
          pendingRelayCount: 0,
          queuedMessages: work.queuedMessages.map(message => ({
            sequence: message.sequence,
            payload: Buffer.from(message.payload)
          })),
          timers: work.timers.map(timer => ({ ...timer }))
        }),
        signal
      );
      return new ServiceCapturedObjectRelocation({
        aggregateId,
        aggregateGeneration,
        sourceCleanup: 'pending',
        participants,
        memberships
      }, units);
    } catch (error) {
      for (const { unit } of captured.reverse()) await unit.abortSeal();
      throw error;
    }
  }
}

export interface ServiceRelocationHiddenObject {
  readonly authorityKey: string;
}

export interface ServiceRelocationTargetObjectPort<
  THidden extends ServiceRelocationHiddenObject
> {
  createHidden(
    participant: ServiceRelocationParticipant,
    signal?: AbortSignal
  ): Promise<THidden>;
  restoreApplicationState(
    hidden: THidden,
    payload: Uint8Array,
    signal?: AbortSignal
  ): Promise<void>;
  restoreMemberships(
    hidden: ReadonlyMap<string, THidden>,
    memberships: readonly ServiceRelocationMembership[],
    signal?: AbortSignal
  ): Promise<void>;
  publish(
    hidden: THidden,
    authority: ZLinkAuthoritySnapshot,
    signal?: AbortSignal
  ): Promise<void>;
  replayAcceptedJournal(
    hidden: THidden,
    payload: Uint8Array,
    signal?: AbortSignal
  ): Promise<ServiceRelocationParticipantProgress | void>;
  replayQueuedMessage(
    hidden: THidden,
    message: ServiceRelocationQueuedMessage,
    signal?: AbortSignal
  ): Promise<void>;
  restoreTimer(
    hidden: THidden,
    timer: ServiceRelocationTimer,
    signal?: AbortSignal
  ): Promise<void>;
  normalize(
    hidden: THidden,
    authority: ZLinkAuthoritySnapshot,
    signal?: AbortSignal
  ): Promise<void>;
  openAdmission(hidden: THidden, signal?: AbortSignal): Promise<void>;
  abort(hidden: THidden): Promise<void> | void;
}

export interface ServiceObjectRelocationStaging<
  THidden extends ServiceRelocationHiddenObject
> extends ServiceRelocationStaging {
  readonly primaryAuthorityKey: ZLinkAuthorityKey;
  readonly envelope: ServiceRelocationEnvelope;
  readonly hidden: ReadonlyMap<string, THidden>;
  readonly successorProgress: ReadonlyMap<string, ServiceRelocationParticipantProgress>;
}

/** Restores an exact User Spot aggregate or standalone Actor into hidden objects. */
export class ServiceRelocationObjectRestoreOwner<
  THidden extends ServiceRelocationHiddenObject
> implements ServiceRelocationRestoreOwner<ServiceObjectRelocationStaging<THidden>> {
  constructor(
    private readonly target: ServiceRelocationTargetObjectPort<THidden>,
    private readonly authorityKey: (value: string) => ZLinkAuthorityKey,
    private readonly maxConcurrentCallbacks = 8
  ) {
    requireCallbackConcurrency(maxConcurrentCallbacks);
  }

  async prepare(
    envelope: ServiceRelocationEnvelope,
    signal?: AbortSignal
  ): Promise<ServiceObjectRelocationStaging<THidden>> {
    validateRelocationShape(envelope);
    const hidden = new Map<string, THidden>();
    try {
      const participants = [...envelope.participants].sort((left, right) => {
        const leftActor = left.objectKind === 'actor' ? 1 : 0;
        const rightActor = right.objectKind === 'actor' ? 1 : 0;
        if (leftActor !== rightActor) return leftActor - rightActor;
        return left.key.localeCompare(right.key);
      });
      for (const participant of participants) {
        signal?.throwIfAborted();
        const created = await this.target.createHidden(participant, signal);
        if (created.authorityKey !== participant.key || hidden.has(created.authorityKey)) {
          throw new Error('Target factory returned a different relocation authority identity.');
        }
        hidden.set(created.authorityKey, created);
      }
      await mapConcurrentOrdered(
        participants,
        this.maxConcurrentCallbacks,
        participant => this.target.restoreApplicationState(
          hidden.get(participant.key)!,
          participant.applicationState,
          signal
        ),
        signal
      );
      await this.target.restoreMemberships(hidden, envelope.memberships, signal);
      const spot = envelope.participants.find(({ objectKind }) =>
        objectKind === 'user_spot' || objectKind === 'instance_spot');
      const primary = spot ?? envelope.participants[0]!;
      return {
        id: `${envelope.aggregateId}:${envelope.aggregateGeneration}`,
        primaryAuthorityKey: this.authorityKey(primary.key),
        envelope,
        hidden,
        successorProgress: new Map()
      };
    } catch (error) {
      for (const value of [...hidden.values()].reverse()) await this.target.abort(value);
      throw error;
    }
  }

  async publish(
    staging: ServiceObjectRelocationStaging<THidden>,
    authority: ZLinkAuthoritySnapshot,
    signal?: AbortSignal
  ): Promise<void> {
    for (const hidden of staging.hidden.values()) {
      await this.target.publish(hidden, authority, signal);
    }
  }

  async replayAcceptedJournal(
    staging: ServiceObjectRelocationStaging<THidden>,
    signal?: AbortSignal
  ): Promise<void> {
    for (const participant of staging.envelope.participants) {
      const hidden = staging.hidden.get(participant.key)!;
      const progress = await this.target.replayAcceptedJournal(
        hidden,
        participant.acceptedJournal,
        signal
      );
      if (progress !== undefined) {
        (staging.successorProgress as Map<string, ServiceRelocationParticipantProgress>)
          .set(participant.key, progress);
      }
      for (const message of participant.queuedMessages) {
        await this.target.replayQueuedMessage(hidden, message, signal);
      }
      for (const timer of participant.timers) {
        await this.target.restoreTimer(hidden, timer, signal);
      }
    }
  }

  async normalize(
    staging: ServiceObjectRelocationStaging<THidden>,
    authority: ZLinkAuthoritySnapshot,
    signal?: AbortSignal
  ): Promise<void> {
    for (const hidden of staging.hidden.values()) {
      await this.target.normalize(hidden, authority, signal);
    }
  }

  async openAdmission(
    staging: ServiceObjectRelocationStaging<THidden>,
    signal?: AbortSignal
  ): Promise<void> {
    for (const hidden of staging.hidden.values()) {
      await this.target.openAdmission(hidden, signal);
    }
  }

  async abort(staging: ServiceObjectRelocationStaging<THidden>): Promise<void> {
    for (const hidden of [...staging.hidden.values()].reverse()) {
      await this.target.abort(hidden);
    }
  }
}

export class ServiceCapturedRelocationSourceCompletion<
  TStaging extends ServiceRelocationStaging
> implements ServiceRelocationSourceCompletion<TStaging> {
  constructor(
    private readonly captured: ServiceCapturedObjectRelocation,
    private readonly durable: ServiceRelocationSourceCompletion<TStaging>
  ) {}

  async complete(
    staging: TStaging,
    authority: ZLinkAuthoritySnapshot,
    signal?: AbortSignal
  ): Promise<ZLinkAuthoritySnapshot> {
    await this.captured.commitSource();
    return await this.durable.complete(staging, authority, signal);
  }
}

function validateRelocationShape(envelope: ServiceRelocationEnvelope): void {
  const spots = envelope.participants.filter(({ objectKind }) =>
    objectKind === 'user_spot' || objectKind === 'instance_spot');
  const actors = envelope.participants.filter(({ objectKind }) => objectKind === 'actor');
  if (spots.length === 0 && (actors.length !== 1 || envelope.participants.length !== 1)) {
    throw new TypeError('Standalone relocation must contain exactly one Actor.');
  }
  if (spots.length === 1 && actors.length + 1 !== envelope.participants.length) {
    throw new TypeError('User Spot aggregate contains an unsupported participant kind.');
  }
  if (spots.length > 1 || envelope.memberships.length !== actors.length) {
    throw new TypeError('Relocation membership inventory does not cover its Actor participants.');
  }
}

function requireCallbackConcurrency(value: number): void {
  if (!Number.isSafeInteger(value) || value <= 0) {
    throw new RangeError('Relocation callback concurrency must be a positive safe integer.');
  }
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
  await Promise.all(
    Array.from({ length: Math.min(concurrency, values.length) }, () => worker())
  );
  if (failure !== undefined) throw failure;
  return results;
}
