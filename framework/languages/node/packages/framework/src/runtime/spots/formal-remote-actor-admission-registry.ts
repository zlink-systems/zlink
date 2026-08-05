import type { ActorRef, RoutingId, ZLinkActor } from '../../contracts';

const ADMISSION_RETENTION_MS = 30_000;

export type ZLinkFormalRemoteActorAdmissionState =
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
}

export interface ZLinkFormalRemoteActorAdmissionOutcome {
  readonly accepted: boolean;
  readonly actorRef: ActorRef;
  readonly reply?: Buffer;
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
  state: ZLinkFormalRemoteActorAdmissionState;
  result?: ZLinkFormalRemoteActorAdmissionResult;
  actor?: ZLinkActor;
}

/** Owns target-side admission, commit tombstones and duplicate request handling. */
export class ZLinkFormalRemoteActorAdmissionRegistry {
  private readonly admissions = new Map<string, ZLinkFormalRemoteActorAdmissionEntry>();

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
      state: 'admitting' as const,
      resultTask,
      timer,
      resolveResult
    } satisfies ZLinkFormalRemoteActorAdmissionEntry;
    this.admissions.set(input.transferId, entry);
    return { record: entry, created: true };
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
      ...(outcome.reply === undefined ? {} : { reply: Buffer.from(outcome.reply) })
    };
    entry.resolveResult(entry.result);
  }

  fail(transferId: string, error: unknown): void {
    const entry = this.admissions.get(transferId);
    if (entry === undefined || entry.state === 'aborted' || entry.state === 'committed') return;
    entry.state = 'failed';
    entry.result = { error };
    entry.resolveResult(entry.result);
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
  }

  delete(transferId: string): void {
    const pending = this.admissions.get(transferId);
    if (pending === undefined) return;
    clearTimeout(pending.timer);
    this.admissions.delete(transferId);
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
    && left.actorRef.actorId === right.actorRef.actorId
    && String(left.actorRef.nodeRid) === String(right.actorRef.nodeRid)
    && left.actorRef.objectGeneration === right.actorRef.objectGeneration;
}
