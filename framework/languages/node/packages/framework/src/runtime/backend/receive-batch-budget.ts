export const RECEIVE_BATCH_MESSAGE_LIMIT = 64;
const RECEIVE_BATCH_BYTE_LIMIT = 4 * 1024 * 1024;
const RECEIVE_BATCH_TIME_LIMIT_MS = 2;

export class ZLinkReceiveBatchBudget {
  private messages = 0;
  private bytes = 0;
  private startedAt = performance.now();

  record(bytes: number): boolean {
    this.messages += 1;
    this.bytes += bytes;
    return (
      this.messages >= RECEIVE_BATCH_MESSAGE_LIMIT ||
      this.bytes >= RECEIVE_BATCH_BYTE_LIMIT ||
      performance.now() - this.startedAt >= RECEIVE_BATCH_TIME_LIMIT_MS
    );
  }

  async yieldAndReset(): Promise<void> {
    await new Promise<void>((resolve) => setImmediate(resolve));
    this.reset();
  }

  reset(): void {
    this.messages = 0;
    this.bytes = 0;
    this.startedAt = performance.now();
  }
}
