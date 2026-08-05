import { Injectable } from '@nestjs/common';
import {
  type ZLinkMessageFlowEvent,
  type ZLinkMessageFlowObserver
} from '@zlink-systems/framework';
import { EvidenceStore } from '../Infrastructure/evidence-store';

@Injectable()
export class EvidenceDispatchErrorObserver implements ZLinkMessageFlowObserver {
  constructor(private readonly evidence: EvidenceStore) {}

  onMessageFlow(flow: ZLinkMessageFlowEvent): void {
    if (flow.outcome !== 'failed') {
      return;
    }

    this.evidence.add(
      'dispatch-error'
      + `|surface=${flow.surface}`
      + `|kind=${flow.messageKind}`
      + `|reason=${flow.reason ?? '<null>'}`
      + `|action=${flow.action ?? '<null>'}`
      + `|packet=${flow.packetName ?? '<null>'}`
      + `|channel=${flow.channelName ?? '<null>'}`
      + `|topic=${flow.topic ?? '<null>'}`
    );
  }
}
