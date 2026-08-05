import path from 'node:path';
import { createRequire } from 'node:module';
import type { EvidenceStore as EvidenceStoreType } from '../evidence-store';

const requireEvidenceStore = createRequire(__filename);
const evidenceStoreModule = requireEvidenceStore(
  path.resolve(__dirname, '../../../..', 'evidence-store.js')
) as { EvidenceStore: typeof EvidenceStoreType };

export const EvidenceStore = evidenceStoreModule.EvidenceStore;
export type EvidenceStore = EvidenceStoreType;
