import { Injectable } from '@nestjs/common';
import type {
  ZLinkFanoutHandler,
  ZLinkPublishMessageContext
} from '@zlink-systems/framework';
import type { LoadEvent } from '../../../Shared/messages';
import { EvidenceStore } from '../Infrastructure/evidence-store';

@Injectable()
export class LoadEventHandler implements ZLinkFanoutHandler<LoadEvent> {
  constructor(private readonly evidence: EvidenceStore) {}

  async handle(event: LoadEvent, context: ZLinkPublishMessageContext): Promise<void> {
    this.evidence.add(
      `load-event|rid=${this.evidence.rid}|run=${event.runId}|seq=${event.sequence}`
      + `|topic=${context.topic}|packet=${context.packetName}`
    );
  }
}
