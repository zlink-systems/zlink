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

export async function closeResources(resources: readonly CloseableResource[]): Promise<void> {
  const failures: unknown[] = [];
  for (const resource of resources) {
    try {
      await resource.close();
    } catch (error) {
      failures.push(error);
    }
  }
  finishResourceCleanup(failures);
}

export function finishResourceCleanup(failures: readonly unknown[]): void {
  if (failures.length > 0) {
    throw new AggregateError(failures, 'Event-loop resource cleanup failed.');
  }
}
