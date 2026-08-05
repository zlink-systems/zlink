import { Injectable } from '@nestjs/common';
import type { ZLinkMessageContext, ZLinkRequestHandler } from '@zlink-systems/framework';
import type { DelayRes, DelayReq } from '../../../Shared/messages';
import { EvidenceStore } from '../Support/evidence-store';

@Injectable()
export class DelayHandler implements ZLinkRequestHandler<DelayReq, DelayRes> {
  constructor(private readonly evidence: EvidenceStore) {}

  async handle(request: DelayReq, context: ZLinkMessageContext): Promise<DelayRes> {
    void context;
    this.evidence.add(`delay-started|rid=${this.evidence.rid}|request=${request.requestId}|marker=${request.marker}|delayMs=${request.delayMs}`);
    await new Promise((resolve) => setTimeout(resolve, request.delayMs));
    this.evidence.add(`delay-completed|rid=${this.evidence.rid}|request=${request.requestId}|marker=${request.marker}`);
    return {
      requestId: request.requestId,
      marker: request.marker,
      nodeRid: this.evidence.rid
    };
  }
}
