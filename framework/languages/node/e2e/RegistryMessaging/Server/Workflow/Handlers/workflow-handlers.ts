import { Injectable } from '@nestjs/common';
import type {
  ZLinkMessageFlowEvent,
  ZLinkMessageFlowObserver,
  ZLinkMessageContext,
  ZLinkRequestHandler
} from '@zlink-systems/framework';
import type { WorkflowRes, WorkflowReq } from '../../../Shared/messages';
import { EvidenceStore } from '../Infrastructure/evidence-store';

@Injectable()
export class WorkflowRequestHandler implements ZLinkRequestHandler<WorkflowReq, WorkflowRes> {
  constructor(private readonly evidence: EvidenceStore) {}

  async handle(request: WorkflowReq, context: ZLinkMessageContext): Promise<WorkflowRes> {
    this.evidence.add(`workflow-request|rid=${this.evidence.rid}|value=${request.value}|packet=${context.packetName}`);
    return { value: `workflow:${request.value}`, providerRid: this.evidence.rid };
  }
}

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
    );
  }
}
