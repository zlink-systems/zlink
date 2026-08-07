import { Injectable } from '@nestjs/common';
import type {
  ZLinkMessageContext,
  ZLinkRequestHandler
} from '@zlink-systems/framework';
import type { WorkflowRes, WorkflowReq } from '../../../Shared/messages';
import { setE2eTelemetryLogReceiver } from '../../../Shared/telemetry-log-provider';
import { EvidenceStore } from '../Infrastructure/evidence-store';

@Injectable()
export class WorkflowRequestHandler implements ZLinkRequestHandler<WorkflowReq, WorkflowRes> {
  constructor(private readonly evidence: EvidenceStore) {}

  async handle(request: WorkflowReq, context: ZLinkMessageContext): Promise<WorkflowRes> {
    this.evidence.add(`workflow-request|rid=${this.evidence.rid}|value=${request.value}|packet=${context.packetName}`);
    return { value: `workflow:${request.value}`, providerRid: this.evidence.rid };
  }
}

export function captureDispatchErrors(evidence: EvidenceStore): void {
  setE2eTelemetryLogReceiver((record) => {
    if (record.eventId !== 'zlink.dispatch_error') return;
    const fields = record.attributes;
    evidence.add(
      'dispatch-error'
      + `|surface=${fields.surface}`
      + `|kind=${fields.message_kind}`
      + `|reason=${fields.reason ?? '<null>'}`
      + `|action=${fields.action ?? '<null>'}`
      + `|packet=${fields.packet_name ?? '<null>'}`
      + `|channel=${fields.channel_name ?? '<null>'}`
    );
  });
}
