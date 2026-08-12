import { Injectable } from '@nestjs/common';
import type {
  ZLinkMessageContext,
  ZLinkRequestHandler,
  ZLinkRouteMessageContext,
  ZLinkRouteRequestHandler,
  ZLinkSendHandler
} from '@zlink-systems/framework';
import type { ChannelEchoRes, ChannelEchoReq, ChannelMsg } from '../../../Shared/messages';
import { EvidenceStore } from '../Infrastructure/evidence-store';

@Injectable()
export class ChannelEchoHandler implements ZLinkRequestHandler<ChannelEchoReq, ChannelEchoRes> {
  constructor(private readonly evidence: EvidenceStore) {}

  async handle(request: ChannelEchoReq, context: ZLinkMessageContext): Promise<ChannelEchoRes> {
    void context;
    this.evidence.add(`channel-echo|value=${request.value}`);
    return { value: `echo-${request.value}` };
  }
}

@Injectable()
export class NodeEchoHandler implements ZLinkRouteRequestHandler<ChannelEchoReq, ChannelEchoRes> {
  constructor(private readonly evidence: EvidenceStore) {}

  async handle(request: ChannelEchoReq, context: ZLinkRouteMessageContext): Promise<ChannelEchoRes> {
    void context;
    this.evidence.add(`node-echo|value=${request.value}`);
    return { value: `echo-${request.value}` };
  }
}

@Injectable()
export class ChannelMsgHandler implements ZLinkSendHandler<ChannelMsg> {
  constructor(private readonly evidence: EvidenceStore) {}

  async handle(message: ChannelMsg, context: ZLinkMessageContext): Promise<void> {
    void context;
    this.evidence.add(`channel-notify|marker=${message.marker}`);
  }
}
