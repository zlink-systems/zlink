import fs from 'node:fs';
import path from 'node:path';

class EvidenceStore {
  private readonly entries: string[] = [];
  private readonly waiters = new Set<() => void>();

  constructor(readonly rid: string, private readonly filePath?: string) {
    if (filePath !== undefined && filePath.length > 0) {
      fs.mkdirSync(path.dirname(filePath), { recursive: true });
      fs.writeFileSync(filePath, '');
    }
  }

  add(entry: string): void {
    this.entries.push(entry);
    if (this.filePath !== undefined && this.filePath.length > 0) {
      fs.appendFileSync(this.filePath, `${entry}\n`);
    }
    const waiters = [...this.waiters];
    this.waiters.clear();
    for (const waiter of waiters) waiter();
  }

  snapshot(): string[] {
    return [...this.entries];
  }

  clear(): void {
    this.entries.length = 0;
    if (this.filePath !== undefined && this.filePath.length > 0) fs.writeFileSync(this.filePath, '');
  }

  async waitUntil(
    condition: readonly string[] | ((entries: readonly string[]) => boolean),
    timeoutMs: number
  ): Promise<string[]> {
    const deadline = Date.now() + timeoutMs;
    while (true) {
      const snapshot = this.snapshot();
      const matched = typeof condition === 'function'
        ? condition(snapshot)
        : condition.every((expected) => snapshot.some((entry) => entry.includes(expected)));
      if (matched) return snapshot;
      const remaining = deadline - Date.now();
      if (remaining <= 0) throw new Error('Timed out waiting for e2e evidence.');
      await new Promise<void>((resolve) => {
        let timer: NodeJS.Timeout;
        const done = (): void => {
          clearTimeout(timer);
          this.waiters.delete(done);
          resolve();
        };
        timer = setTimeout(done, Math.min(remaining, 250));
        this.waiters.add(done);
      });
    }
  }
}

export { EvidenceStore };
