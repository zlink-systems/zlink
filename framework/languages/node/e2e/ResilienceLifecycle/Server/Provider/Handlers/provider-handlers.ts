import { Injectable } from '@nestjs/common';
import type {
  ZLinkMessageFlowEvent,
  ZLinkMessageFlowObserver,
  ZLinkMessageContext,
  ZLinkRequestHandler,
  ZLinkRuntimeErrorEvent,
  ZLinkRuntimeErrorSink,
  ZLinkSendHandler
} from '@zlink-systems/framework';
import type {
  PayloadRes,
  PayloadReq,
  ProfileMsg,
  ProfileRes,
  ProfileReq
} from '../../../Shared/messages';
import { sha256Hex } from '../../../Shared/messages';
import { EvidenceStore } from '../Infrastructure/evidence-store';
import { FaultState } from '../Infrastructure/fault-state';

@Injectable()
export class ProfileRequestHandler implements ZLinkRequestHandler<ProfileReq, ProfileRes> {
  constructor(
    private readonly evidence: EvidenceStore,
    private readonly fault: FaultState
  ) {}

  async handle(request: ProfileReq, context: ZLinkMessageContext): Promise<ProfileRes> {
    const marker = request.marker ?? '<none>';
    this.evidence.add(`profile-start|rid=${this.evidence.rid}|value=${request.value}|marker=${marker}|packet=${context.packetName}`);
    if (this.fault.mode === 'gray' && request.value === 'fast') {
      this.evidence.add(`profile-fault|rid=${this.evidence.rid}|marker=${marker}|mode=gray|packet=${context.packetName}`);
      throw new Error('gray failure');
    }
    if (this.fault.mode === 'crash-on-slow-start' && request.value === 'slow') {
      setTimeout(() => process.exit(134), 50);
    }
    if (request.value === 'slow') {
      await delay(3000);
    }
    if (request.value === 'accepted') {
      await delay(1500);
    }
    this.evidence.add(`profile-request|rid=${this.evidence.rid}|value=${request.value}|marker=${marker}|packet=${context.packetName}`);
    return { value: `profile:${request.value}`, providerRid: this.evidence.rid };
  }
}

@Injectable()
export class ProfileCommandHandler implements ZLinkSendHandler<ProfileMsg> {
  constructor(private readonly evidence: EvidenceStore) {}

  async handle(command: ProfileMsg, context: ZLinkMessageContext): Promise<void> {
    if (command.commandId.startsWith('rl-c9-slow-')) {
      await delay(1000);
    }
    this.evidence.add(
      `profile-command|rid=${this.evidence.rid}|command=${command.commandId}|marker=${command.commandId}|packet=${context.packetName}`
    );
  }
}

@Injectable()
export class PayloadRequestHandler implements ZLinkRequestHandler<PayloadReq, PayloadRes> {
  constructor(private readonly evidence: EvidenceStore) {}

  async handle(request: PayloadReq, context: ZLinkMessageContext): Promise<PayloadRes> {
    const hash = sha256Hex(request.payload);
    this.evidence.add(
      `payload-request|rid=${this.evidence.rid}|marker=${request.marker}`
      + `|length=${request.payload.length}|sha256=${hash}|packet=${context.packetName}`
    );
    return { marker: request.marker, length: request.payload.length, sha256: hash };
  }
}

@Injectable()
export class EvidenceDispatchErrorObserver implements ZLinkMessageFlowObserver {
  constructor(
    private readonly evidence: EvidenceStore,
    private readonly fault: FaultState
  ) {}

  onMessageFlow(flow: ZLinkMessageFlowEvent): void {
    if (flow.outcome !== 'failed') {
      return;
    }
    this.evidence.add(
      'dispatch-error'
      + '|outcome=failed'
      + `|surface=${flow.surface}`
      + `|kind=${flow.messageKind}`
      + `|reason=${flow.reason}`
      + `|action=${flow.action}`
      + `|packet_name=${flow.packetName ?? '<null>'}`
      + `|channel=${flow.channelName ?? '<null>'}`
    );
    if (this.fault.mode === 'observer-throws') {
      throw new Error('dispatch observer failure');
    }
  }
}

@Injectable()
export class EvidenceRuntimeErrorSink implements ZLinkRuntimeErrorSink {
  constructor(private readonly evidence: EvidenceStore) {}

  onRuntimeError(error: ZLinkRuntimeErrorEvent): void {
    this.evidence.add(
      'runtime-error'
      + `|event_id=${error.eventId}`
      + `|kind=${error.kind}`
      + `|source=${error.source}`
      + `|reason=${error.reason}`
      + `|fields=${Object.keys(error).sort().join(',')}`
    );
  }
}

function delay(milliseconds: number): Promise<void> {
  return new Promise((resolve) => setTimeout(resolve, milliseconds));
}
