import { Injectable } from '@nestjs/common';
import type { ZLinkFanoutHandler, ZLinkPublishMessageContext } from '@zlink-systems/framework';
import { FanoutEvent } from '../../../Shared/messages';
import { EvidenceStore } from '../../../evidence-store';

@Injectable()
export class ConsumerFanoutEventHandler implements ZLinkFanoutHandler<FanoutEvent> {
  constructor(private readonly evidence: EvidenceStore) {}

  async handle(event: FanoutEvent, _context: ZLinkPublishMessageContext): Promise<void> {
    this.evidence.add(`fanout-event|value=${event.value}|marker=${event.marker ?? ''}`);
  }
}
