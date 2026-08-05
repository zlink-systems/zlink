export class ZLinkActorSessionLifecycleCoordinator {
  private readonly tails = new Map<string, Promise<void>>();

  async run<T>(actorId: string, operation: () => Promise<T>): Promise<T> {
    const previous = this.tails.get(actorId) ?? Promise.resolve();
    let release!: () => void;
    const current = new Promise<void>((resolve) => { release = resolve; });
    const tail = previous.catch(() => undefined).then(() => current);
    this.tails.set(actorId, tail);
    await previous.catch(() => undefined);
    try {
      return await operation();
    } finally {
      release();
      if (this.tails.get(actorId) === tail) {
        this.tails.delete(actorId);
      }
    }
  }
}
