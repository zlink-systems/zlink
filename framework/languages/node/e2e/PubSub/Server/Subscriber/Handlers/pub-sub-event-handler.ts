import { Inject, Injectable } from '@nestjs/common';
import {
  type ZLinkPublishMessageContext,
  type ZLinkFanoutHandler,
} from '@zlink-systems/framework';
import { PubSubNames, type PubSubEvent } from '../../../Shared/messages';
import { setE2eTelemetryLogReceiver } from '../../../Shared/telemetry-log-provider';
import { SUBSCRIBER_OPTIONS, type SubscriberOptions } from '../Configuration/subscriber-options';
import { EvidenceStore } from '../Infrastructure/evidence-store';

@Injectable()
export class PubSubEventHandler implements ZLinkFanoutHandler<PubSubEvent> {
  constructor(
    private readonly evidence: EvidenceStore,
    @Inject(SUBSCRIBER_OPTIONS)
    private readonly options: SubscriberOptions
  ) {}

  async handle(message: PubSubEvent, context: ZLinkPublishMessageContext): Promise<void> {
    if (this.options.handlerDelayMs > 0 && message.value.startsWith('slow-')) {
      this.evidence.add(
        `delay-start|rid=${this.evidence.rid}|run=${message.runId}|topic=${context.topic}`
        + `|seq=${message.sequence}|value=${message.value}`
      );
      await new Promise((resolve) => setTimeout(resolve, this.options.handlerDelayMs));
    }

    if (context.topic === PubSubNames.mainTopic) {
      this.evidence.add(
        `event|rid=${this.evidence.rid}|run=${message.runId}|topic=${context.topic}`
        + `|seq=${message.sequence}|value=${message.value}|packet=${context.packetName}`
      );
    } else {
      this.evidence.add(
        `ignored|rid=${this.evidence.rid}|run=${message.runId}|topic=${context.topic}`
        + `|seq=${message.sequence}|value=${message.value}|packet=${context.packetName}`
      );
    }
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
      + `|topic=${fields.topic ?? '<null>'}`
    );
  });
}
