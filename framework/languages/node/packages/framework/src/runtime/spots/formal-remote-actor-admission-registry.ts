import type {
  ActorRef,
  RoutingId,
  ZLinkActor,
  ZLinkActorJoinOperationId
} from '../../contracts';
import type { ZLinkRemoteBoundSessionTarget } from '../actors/actor-runtime-state';
import type { ZLinkDeferredJoinAcceptedRoot } from '../actors/deferred-join-accepted-journal';
import type { ZLinkActorRequestTerminal } from './spot-actor-packet-dispatch';

const ADMISSION_RETENTION_MS = 30_000;

/**
 * One production-ingress arrival parked at the target between admission
 * Accepted and the cutover that makes the Actor resolvable (spec 15 §4.2
 * relocation temporary queue). Header/payload are kept as raw bytes — not
 * as the original {@link Message} objects — so parking never depends on
 * how long the underlying buffer stays valid.
 */
export interface ZLinkParkedActorArrival {
  readonly header: Buffer;
  readonly payload: Buffer;
  readonly returnResponse: boolean;
  readonly remoteBoundSessionTarget?: ZLinkRemoteBoundSessionTarget;
  readonly fallbackActorRef?: ActorRef;
  readonly requestTerminal?: ZLinkActorRequestTerminal;
  readonly resolve: (value: unknown) => void;
  readonly reject: (reason: unknown) => void;
}

export type ZLinkActorJoinIngressRoute = 'parked' | 'not-found';

/** Identifies the object one admission attempt belongs to. */
function objectKey(actorId: string, objectGeneration: bigint): string {
  return `${actorId}\u0000${objectGeneration}`;
}

export type ZLinkFormalRemoteActorAdmissionState =
  | 'provisional'
  | 'admitting'
  | 'admitted'
  | 'rejected'
  | 'committed'
  | 'aborted'
  | 'failed';

export interface ZLinkFormalRemoteActorAdmission {
  readonly actorId: string;
  readonly actorType: string;
  readonly actorRef: ActorRef;
  readonly spotId: RoutingId;
  readonly targetSpotGeneration: bigint;
  readonly expectedMembershipEpoch: bigint;
  readonly requestFingerprint: string;
  readonly transferId: string;
  readonly completionOperationId?: ZLinkActorJoinOperationId;
  readonly sourceSpotId?: RoutingId;
  readonly sourceActorNodeRid?: RoutingId;
}

export type ZLinkProvisionalRemoteActorAdmission = Omit<
  ZLinkFormalRemoteActorAdmission,
  'actorType'
>;

export interface ZLinkFormalRemoteActorAdmissionOutcome {
  readonly accepted: boolean;
  readonly actorRef: ActorRef;
  readonly reply?: Buffer;
  readonly replyContentType?: string;
  readonly deferredJoinRoot?: ZLinkDeferredJoinAcceptedRoot;
}

export interface ZLinkFormalRemoteActorAdmissionFailure {
  readonly error: unknown;
}

export type ZLinkFormalRemoteActorAdmissionResult =
  | ZLinkFormalRemoteActorAdmissionOutcome
  | ZLinkFormalRemoteActorAdmissionFailure;

export interface ZLinkFormalRemoteActorAdmissionRecord {
  readonly admission: ZLinkFormalRemoteActorAdmission;
  readonly state: ZLinkFormalRemoteActorAdmissionState;
  readonly result?: ZLinkFormalRemoteActorAdmissionResult;
  readonly actor?: ZLinkActor;
  readonly resultTask: Promise<ZLinkFormalRemoteActorAdmissionResult>;
}

interface ZLinkFormalRemoteActorAdmissionEntry extends ZLinkFormalRemoteActorAdmissionRecord {
  readonly timer: ReturnType<typeof setTimeout>;
  readonly resolveResult: (result: ZLinkFormalRemoteActorAdmissionResult) => void;
  admission: ZLinkFormalRemoteActorAdmission;
  state: ZLinkFormalRemoteActorAdmissionState;
  result?: ZLinkFormalRemoteActorAdmissionResult;
  actor?: ZLinkActor;
  //  Spec 15 §4.2 relocation temporary queue: arrivals for this exact
  //  object parked between Accepted and the cutover that makes the Actor
  //  resolvable through ordinary ingress. Once `migrated` is true the
  //  parked queue has already been drained in order and this attempt no
  //  longer accepts new arrivals — ordinary ingress resolves the Actor
  //  directly from that point on.
  parked: ZLinkParkedActorArrival[];
  migrated: boolean;
}

/** Owns target-side admission, commit tombstones and duplicate request handling. */
export class ZLinkFormalRemoteActorAdmissionRegistry {
  private readonly admissions = new Map<string, ZLinkFormalRemoteActorAdmissionEntry>();
  //  Spec 15 §4.2 "같은 object의 relocation temporary queue는 하나만
  //  존재한다": at most one admission attempt per (actorId,
  //  objectGeneration). A newer exact identity for the same object evicts
  //  the older attempt — newest attempt wins.
  private readonly byObject = new Map<string, string>();

  get(transferId: string): ZLinkFormalRemoteActorAdmissionEntry | undefined {
    return this.admissions.get(transferId);
  }

  begin(input: ZLinkFormalRemoteActorAdmission): {
    readonly record: ZLinkFormalRemoteActorAdmissionEntry;
    readonly created: boolean;
  } {
    const existing = this.admissions.get(input.transferId);
    if (existing !== undefined) {
      if (!sameAdmission(existing.admission, input)) {
        throw new Error(`Remote actor admission '${input.transferId}' does not match the original request.`);
      }
      return { record: existing, created: false };
    }
    return this.create(input, 'admitting');
  }

  /**
   * Installs the exact object/handoff identity synchronously before canonical
   * Store and factory validation yields. The actor type is deliberately not
   * invented while that validation is pending; {@link finalizeProvisional}
   * supplies it once the Store result is available.
   */
  beginProvisional(input: ZLinkProvisionalRemoteActorAdmission): {
    readonly record: ZLinkFormalRemoteActorAdmissionEntry;
    readonly created: boolean;
  } {
    const existing = this.admissions.get(input.transferId);
    if (existing !== undefined) {
      if (!sameProvisionalAdmission(existing.admission, input)) {
        throw new Error(`Remote actor admission '${input.transferId}' does not match the original request.`);
      }
      return { record: existing, created: false };
    }
    return this.create({ ...input, actorType: '' }, 'provisional');
  }

  /** Completes a synchronous provisional registration after async validation. */
  finalizeProvisional(
    transferId: string,
    input: ZLinkFormalRemoteActorAdmission
  ): ZLinkFormalRemoteActorAdmissionEntry {
    const entry = this.admissions.get(transferId);
    if (entry === undefined) {
      throw new Error(`Remote actor admission '${transferId}' was evicted during validation.`);
    }
    if (!sameProvisionalAdmission(entry.admission, input)) {
      throw new Error(`Remote actor admission '${transferId}' changed during validation.`);
    }
    if (entry.state === 'provisional') {
      entry.admission = input;
      entry.state = 'admitting';
    } else if (!sameAdmission(entry.admission, input)) {
      throw new Error(`Remote actor admission '${transferId}' resolved a different Actor type.`);
    }
    return entry;
  }

  private create(
    input: ZLinkFormalRemoteActorAdmission,
    state: 'provisional' | 'admitting'
  ): {
    readonly record: ZLinkFormalRemoteActorAdmissionEntry;
    readonly created: boolean;
  } {
    const key = objectKey(input.actorId, input.actorRef.objectGeneration);
    const displacedTransferId = this.byObject.get(key);
    if (displacedTransferId !== undefined && displacedTransferId !== input.transferId) {
      this.evict(displacedTransferId);
    }
    let resolveResult!: (result: ZLinkFormalRemoteActorAdmissionResult) => void;
    const resultTask = new Promise<ZLinkFormalRemoteActorAdmissionResult>((resolve) => {
      resolveResult = resolve;
    });
    const timer = setTimeout(() => {
      this.delete(input.transferId);
    }, ADMISSION_RETENTION_MS);
    timer.unref();
    const entry = {
      admission: input,
      state,
      resultTask,
      timer,
      resolveResult,
      parked: [],
      migrated: false
    } satisfies ZLinkFormalRemoteActorAdmissionEntry;
    this.admissions.set(input.transferId, entry);
    this.byObject.set(key, input.transferId);
    return { record: entry, created: true };
  }

  /**
   * Production ingress lookup (spec 15 §4.2). Parks the arrival in the
   * relocation temporary queue if an attempt exists for the object and has
   * not yet migrated, or reports not-found so the caller falls through to
   * its existing missing-actor handling — either because no attempt was
   * ever registered for this object, or because the attempt already
   * migrated and the Actor should now resolve directly. Synchronous: the
   * object-key lookup and the park decision are one critical section, so
   * no arrival can slip between lookup and park.
   */
  routeIngress(
    actorId: string,
    objectGeneration: bigint,
    arrival: ZLinkParkedActorArrival
  ): ZLinkActorJoinIngressRoute {
    const transferId = this.byObject.get(objectKey(actorId, objectGeneration));
    if (transferId === undefined) return 'not-found';
    const entry = this.admissions.get(transferId);
    if (
      entry === undefined
      || entry.migrated
      || entry.state === 'rejected'
      || entry.state === 'aborted'
      || entry.state === 'failed'
    ) {
      return 'not-found';
    }
    entry.parked.push(arrival);
    return 'parked';
  }

  /**
   * Installs the migrated state and atomically (synchronously) drains
   * every arrival parked since admission, in order, so the caller can
   * dispatch them for real once the Actor is resolvable (spec 15 §4.2).
   * The drain snapshot is taken and `migrated` flips inside one
   * synchronous critical section — an arrival racing this call sees
   * either the pre-migration parked queue or `migrated === true` and
   * ordinary ingress, never a window where it is silently lost.
   */
  completeMigration(transferId: string): readonly ZLinkParkedActorArrival[] {
    const entry = this.admissions.get(transferId);
    if (entry === undefined) {
      throw new Error(
        `Remote actor admission '${transferId}' was evicted before its relocation temporary queue could migrate.`
      );
    }
    const drained = entry.parked;
    entry.parked = [];
    entry.migrated = true;
    return drained;
  }

  private evict(transferId: string): void {
    if (!this.admissions.has(transferId)) return;
    this.abort(transferId);
    this.delete(transferId);
  }

  private failParked(
    entry: ZLinkFormalRemoteActorAdmissionEntry,
    reason?: unknown
  ): void {
    if (entry.parked.length === 0) return;
    const parked = entry.parked;
    entry.parked = [];
    const aborted = reason ?? new Error(
      `Actor Join relocation temporary queue for '${entry.admission.actorId}' was aborted.`
    );
    for (const message of parked) {
      message.reject(aborted);
    }
  }

  complete(
    transferId: string,
    outcome: ZLinkFormalRemoteActorAdmissionOutcome
  ): void {
    const entry = this.admissions.get(transferId);
    if (entry === undefined || entry.state === 'aborted' || entry.state === 'committed') return;
    entry.state = outcome.accepted ? 'admitted' : 'rejected';
    entry.result = {
      accepted: outcome.accepted,
      actorRef: { ...outcome.actorRef },
      ...(outcome.reply === undefined ? {} : { reply: Buffer.from(outcome.reply) }),
      ...(outcome.replyContentType === undefined
        ? {}
        : { replyContentType: outcome.replyContentType }),
      ...(outcome.deferredJoinRoot === undefined
        ? {}
        : { deferredJoinRoot: outcome.deferredJoinRoot })
    };
    entry.resolveResult(entry.result);
    if (!outcome.accepted) {
      //  Spec 15 §4.2: Rejected admission removes the relocation
      //  temporary queue and fails whatever it was still holding, in the
      //  same processing step as the reject decision.
      this.failParked(entry);
    }
  }

  fail(transferId: string, error: unknown): void {
    const entry = this.admissions.get(transferId);
    if (entry === undefined || entry.state === 'aborted' || entry.state === 'committed') return;
    entry.state = 'failed';
    entry.result = { error };
    entry.resolveResult(entry.result);
    this.failParked(entry, error);
  }

  markCommitted(transferId: string, actor: ZLinkActor): void {
    const entry = this.admissions.get(transferId);
    if (entry === undefined) return;
    if (entry.state !== 'admitted' && entry.state !== 'committed') {
      throw new Error(`Remote actor admission '${transferId}' is not ready to commit.`);
    }
    entry.state = 'committed';
    entry.actor = actor;
  }

  attachDeferredJoinRoot(
    transferId: string,
    root: ZLinkDeferredJoinAcceptedRoot
  ): void {
    const entry = this.admissions.get(transferId);
    if (entry === undefined || entry.state !== 'admitted'
      || entry.result === undefined || 'error' in entry.result || !entry.result.accepted) {
      throw new Error(`Remote actor admission '${transferId}' cannot attach deferred recovery.`);
    }
    entry.result = { ...entry.result, deferredJoinRoot: root };
  }

  abort(transferId: string): void {
    const entry = this.admissions.get(transferId);
    if (entry === undefined || entry.state === 'committed') return;
    entry.state = 'aborted';
    if (entry.result === undefined) {
      entry.result = {
        error: new Error(`Remote actor admission '${transferId}' was aborted.`)
      };
      entry.resolveResult(entry.result);
    }
    //  Spec 15 §4.2: Rejected admission or an explicit target abort
    //  removes the relocation temporary queue and fails whatever it was
    //  still holding, exactly once.
    this.failParked(entry);
  }

  /**
   * Releases the admission for {@code transferId} — normal retention
   * expiry, or explicit cleanup once the transfer is fully resolved. Any
   * arrival still parked at release time fails exactly once (spec 15
   * §4.2), mirroring {@link #abort}.
   */
  delete(transferId: string): void {
    const pending = this.admissions.get(transferId);
    if (pending === undefined) return;
    clearTimeout(pending.timer);
    this.admissions.delete(transferId);
    this.failParked(pending);
    const key = objectKey(pending.admission.actorId, pending.admission.actorRef.objectGeneration);
    if (this.byObject.get(key) === transferId) {
      this.byObject.delete(key);
    }
  }
}

function sameAdmission(
  left: ZLinkFormalRemoteActorAdmission,
  right: ZLinkFormalRemoteActorAdmission
): boolean {
  return left.actorId === right.actorId
    && left.actorType === right.actorType
    && String(left.spotId) === String(right.spotId)
    && left.targetSpotGeneration === right.targetSpotGeneration
    && left.expectedMembershipEpoch === right.expectedMembershipEpoch
    && left.requestFingerprint === right.requestFingerprint
    && sameOperationId(left.completionOperationId, right.completionOperationId)
    && String(left.sourceSpotId ?? '') === String(right.sourceSpotId ?? '')
    && String(left.sourceActorNodeRid ?? '') === String(right.sourceActorNodeRid ?? '')
    && left.actorRef.actorId === right.actorRef.actorId
    && String(left.actorRef.nodeRid) === String(right.actorRef.nodeRid)
    && left.actorRef.objectGeneration === right.actorRef.objectGeneration;
}

function sameProvisionalAdmission(
  left: ZLinkFormalRemoteActorAdmission,
  right: ZLinkProvisionalRemoteActorAdmission
): boolean {
  return left.actorId === right.actorId
    && String(left.spotId) === String(right.spotId)
    && left.targetSpotGeneration === right.targetSpotGeneration
    && left.expectedMembershipEpoch === right.expectedMembershipEpoch
    && left.requestFingerprint === right.requestFingerprint
    && sameOperationId(left.completionOperationId, right.completionOperationId)
    && String(left.sourceSpotId ?? '') === String(right.sourceSpotId ?? '')
    && String(left.sourceActorNodeRid ?? '') === String(right.sourceActorNodeRid ?? '')
    && left.actorRef.actorId === right.actorRef.actorId
    && String(left.actorRef.nodeRid) === String(right.actorRef.nodeRid)
    && left.actorRef.objectGeneration === right.actorRef.objectGeneration;
}

function sameOperationId(
  left: ZLinkActorJoinOperationId | undefined,
  right: ZLinkActorJoinOperationId | undefined
): boolean {
  return left === undefined || right === undefined
    ? left === right
    : left.high === right.high && left.low === right.low;
}
