export class RuntimeDisposal {
  private task?: Promise<void> | null;

  get started(): boolean {
    return this.task !== undefined;
  }

  run(close: () => Promise<void>): Promise<void> {
    if (this.task != null) return this.task;
    const task = Promise.resolve()
      .then(close)
      .catch((error: unknown) => {
        this.task = null;
        throw error;
      });
    this.task = task;
    return task;
  }
}
