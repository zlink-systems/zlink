import { Injectable } from '@nestjs/common';
import type {
  ZLinkMessageFlowEvent,
  ZLinkMessageFlowObserver,
  ZLinkMessageContext,
  ZLinkRequestHandler,
  ZLinkRouteMessageContext,
  ZLinkRouteRequestHandler,
  ZLinkSendHandler
} from '@zlink-systems/framework';
import type {
  PayloadRes,
  PayloadReq,
  ProfileMsg,
  ProfileRes,
  ProfileReq,
  ScenarioRouteReq,
  ScenarioRouteRes
} from '../../../Shared/messages';
import { sha256Hex } from '../../../Shared/messages';
import { EvidenceStore } from '../Infrastructure/evidence-store';

@Injectable()
export class ProfileRequestHandler implements ZLinkRequestHandler<ProfileReq, ProfileRes> {
  constructor(private readonly evidence: EvidenceStore) {}

  async handle(request: ProfileReq, context: ZLinkMessageContext): Promise<ProfileRes> {
    if (request.value === 'slow') {
      await delay(1000);
    }
    if (request.value.startsWith('rm-b3-transition-')) {
      this.evidence.add(`profile-request-start|rid=${this.evidence.rid}|value=${request.value}`);
      await delay(1000);
    }
    this.evidence.add(`profile-request|rid=${this.evidence.rid}|value=${request.value}|packet=${context.packetName}`);
    return { value: `profile:${request.value}`, providerRid: this.evidence.rid };
  }
}

@Injectable()
export class ProfileCommandHandler implements ZLinkSendHandler<ProfileMsg> {
  constructor(private readonly evidence: EvidenceStore) {}

  async handle(command: ProfileMsg, context: ZLinkMessageContext): Promise<void> {
    if (command.commandId.startsWith('rm-c9-slow-')) {
      await delay(1000);
    }
    this.evidence.add(`profile-command|rid=${this.evidence.rid}|command=${command.commandId}|packet=${context.packetName}`);
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
export class RoutePingHandler implements ZLinkRouteRequestHandler<ScenarioRouteReq, ScenarioRouteRes> {
  constructor(private readonly evidence: EvidenceStore) {}

  async handle(request: ScenarioRouteReq, context: ZLinkRouteMessageContext): Promise<ScenarioRouteRes> {
    const sourceRid = String(context.sourceNodeRid);
    this.evidence.add(`route-request|rid=${this.evidence.rid}|source=${sourceRid}|value=${request.value}`);
    return { value: `route:${request.value}`, providerRid: this.evidence.rid, sourceRid };
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

function delay(milliseconds: number): Promise<void> {
  return new Promise((resolve) => setTimeout(resolve, milliseconds));
}
