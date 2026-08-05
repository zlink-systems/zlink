export interface CloseableResource {
  close(): void | Promise<void>;
}

/** Closes owned runtime resources once, in reverse acquisition order. */
export class EventLoopResourceStack {
  private readonly resources: CloseableResource[] = [];
  private closePromise?: Promise<void>;

  own<T extends CloseableResource>(resource: T): T {
    if (this.closePromise !== undefined) throw new Error('Resource stack is closing.');
    this.resources.push(resource);
    return resource;
  }

  close(): Promise<void> {
    if (this.closePromise !== undefined) return this.closePromise;
    const resources = this.resources.splice(0).reverse();
    this.closePromise = closeResources(resources);
    return this.closePromise;
  }
}

async function closeResources(resources: readonly CloseableResource[]): Promise<void> {
  const failures: unknown[] = [];
  for (const resource of resources) {
    try {
      await resource.close();
    } catch (error) {
      failures.push(error);
    }
  }
  if (failures.length > 0) {
    throw new AggregateError(failures, 'Event-loop resource cleanup failed.');
  }
}

export type EventLoopTask = () => void | Promise<void>;

/** Keeps infrastructure progress independent from awaiting application handlers. */
export class EventLoopWorkQueues {
  private readonly infrastructure: Array<EventLoopTask | undefined> = [];
  private infrastructureHead = 0;
  private infrastructureCount = 0;
  private readonly application: Array<EventLoopTask | undefined> = [];
  private applicationHead = 0;
  private applicationCount = 0;
  private infrastructureScheduled = false;
  private applicationScheduled = false;
  private accepting = true;

  constructor(
    private readonly infrastructureLimit: number,
    private readonly applicationLimit: number
  ) {
    if (infrastructureLimit < 1 || applicationLimit < 1) {
      throw new RangeError('Queue limits must be positive.');
    }
  }

  submitInfrastructure(task: EventLoopTask): boolean {
    if (!this.accepting || this.infrastructureCount >= this.infrastructureLimit) return false;
    this.infrastructure.push(task);
    this.infrastructureCount += 1;
    this.scheduleInfrastructure();
    return true;
  }

  submitApplication(task: EventLoopTask): boolean {
    if (!this.accepting || this.applicationCount >= this.applicationLimit) return false;
    this.application.push(task);
    this.applicationCount += 1;
    this.scheduleApplication();
    return true;
  }

  stopAdmission(): void {
    this.accepting = false;
  }

  private scheduleInfrastructure(): void {
    if (this.infrastructureScheduled) return;
    this.infrastructureScheduled = true;
    queueMicrotask(() => this.drainInfrastructure());
  }

  private drainInfrastructure(): void {
    this.infrastructureScheduled = false;
    const task = this.takeInfrastructure();
    if (task === undefined) return;
    try {
      const result = task();
      if (result instanceof Promise) void result.catch(() => undefined);
    } finally {
      if (this.infrastructureCount > 0) this.scheduleInfrastructure();
    }
  }

  private scheduleApplication(): void {
    if (this.applicationScheduled) return;
    this.applicationScheduled = true;
    queueMicrotask(() => void this.drainApplication());
  }

  private async drainApplication(): Promise<void> {
    try {
      const task = this.takeApplication();
      if (task !== undefined) await task();
    } catch {
      // Handler failure is reported by the dispatch owner; it does not stop this queue.
    } finally {
      this.applicationScheduled = false;
      if (this.applicationCount > 0) this.scheduleApplication();
    }
  }

  private takeInfrastructure(): EventLoopTask | undefined {
    const task = this.infrastructure[this.infrastructureHead];
    if (task === undefined) return undefined;
    this.infrastructure[this.infrastructureHead] = undefined;
    this.infrastructureHead += 1;
    this.infrastructureCount -= 1;
    this.compactInfrastructure();
    return task;
  }

  private takeApplication(): EventLoopTask | undefined {
    const task = this.application[this.applicationHead];
    if (task === undefined) return undefined;
    this.application[this.applicationHead] = undefined;
    this.applicationHead += 1;
    this.applicationCount -= 1;
    this.compactApplication();
    return task;
  }

  private compactInfrastructure(): void {
    if (this.infrastructureCount === 0) {
      this.infrastructure.length = 0;
      this.infrastructureHead = 0;
    } else if (this.infrastructureHead >= 1024
      && this.infrastructureHead * 2 >= this.infrastructure.length) {
      this.infrastructure.splice(0, this.infrastructureHead);
      this.infrastructureHead = 0;
    }
  }

  private compactApplication(): void {
    if (this.applicationCount === 0) {
      this.application.length = 0;
      this.applicationHead = 0;
    } else if (this.applicationHead >= 1024
      && this.applicationHead * 2 >= this.application.length) {
      this.application.splice(0, this.applicationHead);
      this.applicationHead = 0;
    }
  }
}
