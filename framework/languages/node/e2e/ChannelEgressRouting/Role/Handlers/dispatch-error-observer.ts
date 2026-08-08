import { setE2eTelemetryLogReceiver } from '../../Shared/telemetry-log-provider';
import { EvidenceStore } from '../Support/evidence-store';

export function captureDispatchErrors(evidence: EvidenceStore): void {
  setE2eTelemetryLogReceiver((record) => {
    if (record.eventId !== 'zlink.dispatch_error') return;
    const fields = record.attributes;
    evidence.add(
      `dispatch-error|surface=${fields.surface}|kind=${fields.message_kind}`
      + `|reason=${fields.reason ?? '<null>'}|action=${fields.action ?? '<null>'}`
      + `|packet=${fields.packet_name ?? '<null>'}|channel=${fields.channel_name ?? '<null>'}`
    );
  });
}
