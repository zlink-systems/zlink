import type { RoutingId } from '../../contracts';
import type { ZLinkLocationLifecycle } from '../locations';
import { ZLinkActorRetryDelay } from './actor-retry-delay';

export interface ZLinkPostCommitActorLocationOptions {
  readonly lifecycle: ZLinkLocationLifecycle;
  readonly reportError?: (error: unknown) => void;
  readonly signal?: AbortSignal;
}

interface ActorOperationQueue {
  readonly operations: Array<(() => Promise<void>) | undefined>;
  head: number;
  count: number;
}

export class ZLinkPostCommitActorLocation {
  private readonly queues = new Map<string, ActorOperationQueue>();
  private readonly tasks = new Map<string, Promise<void>>();

  constructor(private readonly options: ZLinkPostCommitActorLocationOptions) {}

  joinedEventually(
    actorType: string,
    actorId: string,
    meshName: string,
    spotId: RoutingId,
    spotGeneration: bigint,
    membershipEpoch: bigint,
    ownerNodeGeneration: bigint
  ): void {
    this.enqueue(actorId, () => this.options.lifecycle.notifyActorJoinedSpot(
      actorType,
      actorId,
      meshName,
      spotId,
      spotGeneration,
      membershipEpoch,
      ownerNodeGeneration
    ));
  }

  leftEventually(
    actorType: string,
    actorId: string,
    entrySpotId: RoutingId,
    entrySpotGeneration: bigint,
    membershipEpoch: bigint,
    ownerNodeGeneration: bigint
  ): void {
    this.enqueue(actorId, () => this.options.lifecycle.notifyActorLeftSpot(
      actorType,
      actorId,
      entrySpotId,
      entrySpotGeneration,
      membershipEpoch,
      ownerNodeGeneration
    ));
  }

  private enqueue(actorId: string, operation: () => Promise<void>): void {
    const queue = this.queues.get(actorId) ?? { operations: [], head: 0, count: 0 };
    queue.operations.push(operation);
    queue.count += 1;
    this.queues.set(actorId, queue);
    if (this.tasks.has(actorId)) {
      return;
    }
    const task = this.run(actorId).finally(() => this.tasks.delete(actorId));
    this.tasks.set(actorId, task);
  }

  private async run(actorId: string): Promise<void> {
    const retryDelay = new ZLinkActorRetryDelay();
    while (this.options.signal?.aborted !== true) {
      const queue = this.queues.get(actorId);
      const operation = queue?.operations[queue.head];
      if (queue === undefined || operation === undefined) {
        this.queues.delete(actorId);
        return;
      }
      try {
        await operation();
        this.complete(queue);
        retryDelay.reset();
      } catch (error) {
        this.options.reportError?.(error);
        if (!await retryDelay.wait(this.options.signal)) {
          return;
        }
      }
    }
  }

  private complete(queue: ActorOperationQueue): void {
    queue.operations[queue.head] = undefined;
    queue.head += 1;
    queue.count -= 1;
    if (queue.count === 0) {
      queue.operations.length = 0;
      queue.head = 0;
    } else if (queue.head >= 1024 && queue.head * 2 >= queue.operations.length) {
      queue.operations.splice(0, queue.head);
      queue.head = 0;
    }
  }
}
