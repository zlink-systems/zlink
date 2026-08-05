import type { ActorRef } from '../../contracts';
import { ZLinkActorRetryDelay } from './actor-retry-delay';

export interface ZLinkPostCommitActorBinderOptions {
  readonly bind: (actorRef: ActorRef, force: boolean) => Promise<void>;
  readonly reportError?: (error: unknown) => void;
  readonly signal?: AbortSignal | (() => AbortSignal | undefined);
}

export class ZLinkPostCommitActorBinder {
  private readonly desiredRefs = new Map<string, ActorRef>();
  private readonly tasks = new Map<string, Promise<void>>();

  constructor(private readonly options: ZLinkPostCommitActorBinderOptions) {}

  bindEventually(actorRef: ActorRef): void {
    void this.bind(actorRef);
  }

  bind(actorRef: ActorRef): Promise<void> {
    this.desiredRefs.set(actorRef.actorId, actorRef);
    const existing = this.tasks.get(actorRef.actorId);
    if (existing !== undefined) return existing;
    const task = this.run(actorRef.actorId)
      .finally(() => this.tasks.delete(actorRef.actorId));
    this.tasks.set(actorRef.actorId, task);
    return task;
  }

  private async run(actorId: string): Promise<void> {
    const retryDelay = new ZLinkActorRetryDelay();
    for (;;) {
      const signal = this.signal();
      if (signal?.aborted === true) return;
      const actorRef = this.desiredRefs.get(actorId);
      if (actorRef === undefined) {
        return;
      }
      try {
        await this.options.bind(actorRef, true);
        if (this.desiredRefs.get(actorId) === actorRef) {
          this.desiredRefs.delete(actorId);
          return;
        }
        retryDelay.reset();
      } catch (error) {
        this.options.reportError?.(error);
        if (!await retryDelay.wait(signal)) return;
      }
    }
  }

  private signal(): AbortSignal | undefined {
    return typeof this.options.signal === 'function'
      ? this.options.signal()
      : this.options.signal;
  }
}
