export declare class EvidenceStore {
  readonly rid: string;
  constructor(rid: string, filePath?: string);
  add(entry: string): void;
  snapshot(): string[];
  clear(): void;
  waitUntil(containsAll: readonly string[], timeoutMs: number): Promise<string[]>;
  waitUntil(predicate: (entries: readonly string[]) => boolean, timeoutMs: number): Promise<string[]>;
}
