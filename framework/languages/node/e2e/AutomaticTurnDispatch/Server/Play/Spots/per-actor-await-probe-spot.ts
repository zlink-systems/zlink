import { Injectable, Scope } from '@nestjs/common';
import { EvidenceStore } from '../Support/evidence-store';
import { AwaitProbeSpot } from './await-probe-spot';

@Injectable({ scope: Scope.TRANSIENT })
export class PerActorAwaitProbeSpot extends AwaitProbeSpot {
  constructor(evidence: EvidenceStore) {
    super(evidence);
  }
}
