import type {
  ApplicationJobPermitPort,
  ApplicationJobQueuePort
} from './contracts';

export type ApplicationJobRecordDomain = 'application' | 'infrastructure';

export interface RetainedIngressRecordPort {
  close(): void;
}

/**
 * Owns one raw ingress record from pre-receive reservation through every
 * derived mailbox job. Child jobs have independent queue permits while the
 * retained Core credit is shared and returned after the last child terminal.
 */
export class ApplicationIngressRecordOwner {
  private readonly stop = new AbortController();
  private closed = false;

  private constructor(private readonly shared: SharedIngressRecordState) {}

  static create(
    queue: ApplicationJobQueuePort,
    initialPermit: ApplicationJobPermitPort,
    retainedRecord: RetainedIngressRecordPort
  ): ApplicationIngressRecordOwner {
    return new ApplicationIngressRecordOwner(
      new SharedIngressRecordState(queue, initialPermit, retainedRecord)
    );
  }

  retain(): ApplicationIngressRecordOwner {
    this.requireOpen();
    this.shared.retainOwner();
    return new ApplicationIngressRecordOwner(this.shared);
  }

  takeInitial(domain: ApplicationJobRecordDomain): ApplicationJobRecordLease {
    this.requireOpen();
    const job = this.shared.takeInitialPermit(domain);
    if (job === undefined) {
      throw new Error('Application ingress record initial permit was already transferred.');
    }
    return job;
  }

  async acquire(
    domain: ApplicationJobRecordDomain,
    signal?: AbortSignal
  ): Promise<ApplicationJobRecordLease> {
    this.requireOpen();
    const initial = this.shared.takeInitialPermit(domain);
    if (initial !== undefined) return initial;

    const waitSignal = signal === undefined
      ? this.stop.signal
      : AbortSignal.any([this.stop.signal, signal]);
    const permit = await this.shared.acquirePermit(waitSignal);
    if (this.closed) {
      permit.releaseAfterInternalProcessing();
      throw ownerClosedError();
    }
    return this.shared.createJob(permit, domain);
  }

  close(): void {
    if (this.closed) return;
    this.closed = true;
    this.stop.abort(ownerClosedError());
    this.shared.releaseOwner();
  }

  private requireOpen(): void {
    if (this.closed) throw ownerClosedError();
  }
}

/** Queue-permit child of one retained raw ingress record. */
export class ApplicationJobRecordLease implements ApplicationJobPermitPort {
  private ingressClosed = false;
  private permitReleased = false;
  private processingTerminal = false;
  private detachedForHandlerTurn = false;
  private jobReleased = false;

  constructor(
    private readonly shared: SharedIngressRecordState,
    private readonly permit: ApplicationJobPermitPort
  ) {}

  markApplicationQueued(): void {
    // The owner classifies and marks the child atomically when it is created;
    // downstream dispatch must not mutate queue accounting a second time.
  }

  detachForHandlerTurn(): void {
    this.detachedForHandlerTurn = true;
  }

  releaseBeforeHandler(): void {
    this.releasePermit(() => this.permit.releaseBeforeHandler());
  }

  releaseAfterInternalProcessing(): void {
    this.processingTerminal = true;
    this.releasePermit(() => this.permit.releaseAfterInternalProcessing());
    this.releaseJobIfTerminal();
  }

  close(): void {
    if (this.ingressClosed) return;
    this.ingressClosed = true;
    if (!this.detachedForHandlerTurn) {
      this.releaseAfterInternalProcessing();
      return;
    }
    this.releaseJobIfTerminal();
  }

  private releasePermit(release: () => void): void {
    if (this.permitReleased) return;
    this.permitReleased = true;
    release();
  }

  private releaseJobIfTerminal(): void {
    if (
      this.jobReleased
      || !this.ingressClosed
      || !this.processingTerminal
    ) return;
    this.jobReleased = true;
    this.shared.releaseJob();
  }
}

class SharedIngressRecordState {
  private ownerCount = 1;
  private jobCount = 0;
  private initialPermit?: ApplicationJobPermitPort;
  private retainedClosed = false;

  constructor(
    private readonly queue: ApplicationJobQueuePort,
    initialPermit: ApplicationJobPermitPort,
    private readonly retainedRecord: RetainedIngressRecordPort
  ) {
    this.initialPermit = initialPermit;
  }

  retainOwner(): void {
    if (this.retainedClosed) throw ownerClosedError();
    this.ownerCount += 1;
  }

  releaseOwner(): void {
    if (this.ownerCount === 0) return;
    this.ownerCount -= 1;
    this.tryCloseRetainedRecord();
  }

  takeInitialPermit(
    domain: ApplicationJobRecordDomain
  ): ApplicationJobRecordLease | undefined {
    const permit = this.initialPermit;
    if (permit === undefined) return undefined;
    this.initialPermit = undefined;
    return this.createJob(permit, domain);
  }

  acquirePermit(signal: AbortSignal): Promise<ApplicationJobPermitPort> {
    return this.queue.acquire(signal);
  }

  createJob(
    permit: ApplicationJobPermitPort,
    domain: ApplicationJobRecordDomain
  ): ApplicationJobRecordLease {
    this.jobCount += 1;
    try {
      if (domain === 'application') permit.markApplicationQueued();
      return new ApplicationJobRecordLease(this, permit);
    } catch (error) {
      this.jobCount -= 1;
      permit.releaseAfterInternalProcessing();
      this.tryCloseRetainedRecord();
      throw error;
    }
  }

  releaseJob(): void {
    if (this.jobCount === 0) {
      throw new Error('Application ingress record job accounting underflow.');
    }
    this.jobCount -= 1;
    this.tryCloseRetainedRecord();
  }

  private tryCloseRetainedRecord(): void {
    if (this.ownerCount !== 0 || this.jobCount !== 0 || this.retainedClosed) return;
    this.retainedClosed = true;
    try {
      this.initialPermit?.releaseAfterInternalProcessing();
    } finally {
      this.initialPermit = undefined;
      this.retainedRecord.close();
    }
  }
}

function ownerClosedError(): Error {
  const error = new Error('Application ingress record owner is closed.');
  error.name = 'AbortError';
  return error;
}
