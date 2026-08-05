import * as fs from 'node:fs';

export class EvidenceStore {
  readonly rid: string;
  readonly role: string;
  readonly instanceMarker: string;
  private readonly entries: string[] = [];
  private readonly file?: string;

  constructor(rid: string, role: string, instanceMarker: string, file?: string) {
    this.rid = rid;
    this.role = role;
    this.instanceMarker = instanceMarker;
    this.file = file;
    if (file !== undefined) fs.mkdirSync(file.slice(0, file.lastIndexOf('/')), { recursive: true });
  }

  add(entry: string): void {
    this.entries.push(entry);
    if (this.file !== undefined) fs.appendFileSync(this.file, `${entry}\n`);
  }

  snapshot(): readonly string[] {
    return [...this.entries];
  }

  waitUntil(predicate: (entries: readonly string[]) => boolean, timeoutMs: number): Promise<readonly string[]> {
    const deadline = Date.now() + timeoutMs;
    return new Promise((resolve, reject) => {
      const poll = (): void => {
        const current = this.snapshot();
        if (predicate(current)) {
          resolve(current);
          return;
        }
        if (Date.now() >= deadline) {
          reject(new Error(`Evidence condition timed out for ${this.rid}.`));
          return;
        }
        setTimeout(poll, 20);
      };
      poll();
    });
  }
}
