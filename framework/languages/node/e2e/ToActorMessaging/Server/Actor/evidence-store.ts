import fs from 'node:fs';
import path from 'node:path';
import type { ActorEvidence } from '../../Shared/messages';

export class EvidenceStore {
  private readonly items: ActorEvidence[] = [];

  constructor(private readonly file: string) {}

  append(evidence: ActorEvidence): void {
    this.items.push(evidence);
    fs.mkdirSync(path.dirname(this.file), { recursive: true });
    fs.appendFileSync(this.file, `${evidence.scenario}|${evidence.actorId}|${evidence.kind}|${evidence.value}\n`);
  }

  all(): readonly ActorEvidence[] {
    return [...this.items];
  }
}
