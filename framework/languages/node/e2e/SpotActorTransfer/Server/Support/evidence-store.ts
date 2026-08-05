import fs from 'node:fs';
import path from 'node:path';
import type { ActorEvidence } from '../../Shared/messages';

export class EvidenceStore {
  private readonly entries: ActorEvidence[] = [];
  private readonly transferIds = new Map<string, string>();
  private sequence = 0;

  constructor(readonly nodeRid: string, private readonly file: string) {}

  correlate(actorId: string, transferId: string | undefined): void {
    if (transferId !== undefined) this.transferIds.set(actorId, transferId);
  }

  add(scenario: string, actorId: string, kind: string, value: string): void {
    const entry = {
      scenario,
      actorId,
      kind,
      value,
      nodeRid: this.nodeRid,
      atNs: process.hrtime.bigint().toString(),
      sequence: this.sequence++,
      transferId: this.transferIds.get(actorId)
    };
    this.entries.push(entry);
    fs.mkdirSync(path.dirname(this.file), { recursive: true });
    fs.appendFileSync(this.file, `${evidenceText(entry)}\n`);
  }

  snapshot(): readonly ActorEvidence[] {
    return [...this.entries];
  }

  async waitUntil(containsAll: readonly string[], timeoutMs: number): Promise<readonly ActorEvidence[]> {
    const deadline = Date.now() + timeoutMs;
    while (Date.now() < deadline) {
      const snapshot = this.snapshot();
      if (containsAll.every((expected) => snapshot.some((entry) => evidenceText(entry).includes(expected)))) {
        return snapshot;
      }
      await new Promise((resolve) => setTimeout(resolve, 50));
    }
    return this.snapshot();
  }
}

export function evidenceText(entry: ActorEvidence): string {
  return `${entry.scenario}|${entry.actorId}|${entry.kind}|${entry.value}|${entry.nodeRid}|transfer=${entry.transferId ?? '<none>'}`;
}
