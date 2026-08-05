import { Injectable } from '@nestjs/common';
import {
  zlinkRequestHandler,
  zlinkSendHandler
} from '@zlink-systems/nestjs';
import type {
  ZLinkMessageContext,
  ZLinkRequestHandler,
  ZLinkSendHandler
} from '@zlink-systems/framework';
import { PacketNames, type EchoMsg, type EchoRes, type EchoReq } from '../../../Shared/messages';
import { EvidenceStore } from '../Infrastructure/evidence-store';

@zlinkRequestHandler('auto', PacketNames.echoAutoReq)
@Injectable()
export class EchoAutoRequestHandler implements ZLinkRequestHandler<EchoReq, EchoRes> {
  constructor(private readonly evidence: EvidenceStore) {}

  async handle(request: EchoReq, context: ZLinkMessageContext): Promise<EchoRes> {
    this.evidence.add(`echo-request|variant=auto|value=${request.value}|content=${context.contentType}`);
    return { value: `echo:${request.value}`, contentType: context.contentType ?? '<null>' };
  }
}

@zlinkSendHandler('auto', PacketNames.echoAutoMsg)
@Injectable()
export class EchoAutoCommandHandler implements ZLinkSendHandler<EchoMsg> {
  constructor(private readonly evidence: EvidenceStore) {}

  async handle(message: EchoMsg, context: ZLinkMessageContext): Promise<void> {
    this.evidence.add(`echo-command|variant=auto|id=${message.commandId}|value=${message.value}|content=${context.contentType}`);
  }
}

@zlinkRequestHandler('attr', PacketNames.echoAttrReq)
@Injectable()
export class EchoAttrRequestHandler implements ZLinkRequestHandler<EchoReq, EchoRes> {
  constructor(private readonly evidence: EvidenceStore) {}

  async handle(request: EchoReq, context: ZLinkMessageContext): Promise<EchoRes> {
    this.evidence.add(`echo-request|variant=attr|value=${request.value}|content=${context.contentType}`);
    return { value: `echo:${request.value}`, contentType: context.contentType ?? '<null>' };
  }
}

@zlinkSendHandler('attr', PacketNames.echoAttrMsg)
@Injectable()
export class EchoAttrCommandHandler implements ZLinkSendHandler<EchoMsg> {
  constructor(private readonly evidence: EvidenceStore) {}

  async handle(message: EchoMsg, context: ZLinkMessageContext): Promise<void> {
    this.evidence.add(`echo-command|variant=attr|id=${message.commandId}|value=${message.value}|content=${context.contentType}`);
  }
}

@Injectable()
export class EchoManualRequestHandler implements ZLinkRequestHandler<EchoReq, EchoRes> {
  constructor(private readonly evidence: EvidenceStore) {}

  async handle(request: EchoReq, context: ZLinkMessageContext): Promise<EchoRes> {
    this.evidence.add(`echo-request|variant=manual|value=${request.value}|content=${context.contentType}`);
    return { value: `echo:${request.value}`, contentType: context.contentType ?? '<null>' };
  }
}

@Injectable()
export class EchoManualCommandHandler implements ZLinkSendHandler<EchoMsg> {
  constructor(private readonly evidence: EvidenceStore) {}

  async handle(message: EchoMsg, context: ZLinkMessageContext): Promise<void> {
    this.evidence.add(`echo-command|variant=manual|id=${message.commandId}|value=${message.value}|content=${context.contentType}`);
  }
}

@Injectable()
export class DuplicateEchoRequestHandler implements ZLinkRequestHandler<EchoReq, EchoRes> {
  async handle(request: EchoReq): Promise<EchoRes> {
    return { value: request.value, contentType: 'duplicate' };
  }
}
