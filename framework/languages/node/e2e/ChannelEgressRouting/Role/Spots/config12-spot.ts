import { Injectable, Scope } from '@nestjs/common';
import type { ZLinkInstanceSpot, ZLinkInstanceSpotContext } from '@zlink-systems/framework';
import { SpotWorkflowHandler } from '../Handlers/spot-handlers';
import { EvidenceStore } from '../Support/evidence-store';

@Injectable({ scope: Scope.TRANSIENT })
export class Config12Spot implements ZLinkInstanceSpot {
  readonly context!: ZLinkInstanceSpotContext;

  constructor(private readonly evidence: EvidenceStore) {}

  configure(): void {
    this.context.handlers.addPacket(SpotWorkflowHandler);
  }

  async onInitialize(): Promise<void> {
    this.evidence.add(`spot-initialize|rid=${this.evidence.rid}|spot=${this.context.spotId}`);
  }

  async onClosing(): Promise<void> {
    this.evidence.add(`spot-closing|rid=${this.evidence.rid}|spot=${this.context.spotId}`);
  }
}
