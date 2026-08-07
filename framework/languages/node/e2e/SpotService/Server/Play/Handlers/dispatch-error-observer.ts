import { setE2eTelemetryLogReceiver } from '../../../Shared/telemetry-log-provider';
import { EvidenceStore } from '../Infrastructure/evidence-store';

export function captureDispatchErrors(evidence: EvidenceStore): void {
  setE2eTelemetryLogReceiver((record) => {
    if (record.eventId !== 'zlink.dispatch_error') return;
    const fields = record.attributes;
    evidence.add(
      'dispatch-error'
      + `|surface=${fields.surface}`
      + `|kind=${fields.message_kind}`
      + `|reason=${fields.reason}`
      + `|action=${fields.action}`
      + `|packet=${fields.packet_name ?? '<null>'}`
      + `|channel=${fields.channel_name ?? '<null>'}`
    );
  });
}
