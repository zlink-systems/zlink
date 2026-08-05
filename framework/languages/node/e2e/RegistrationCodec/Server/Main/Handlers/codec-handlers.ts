import { Injectable } from '@nestjs/common';
import type {
  ZLinkMessageContext,
  ZLinkRequestHandler,
  ZLinkSendHandler
} from '@zlink-systems/framework';
import {
  MessagePackEchoReq,
  ProtobufEchoReq,
  type EchoMsg,
  type EchoRes,
  type EchoReq,
  type MessagePackEchoMsg,
  type ProtobufEchoMsg
} from '../../../Shared/messages';
import { EvidenceStore } from '../Infrastructure/evidence-store';

@Injectable()
export class JsonEchoRequestHandler implements ZLinkRequestHandler<EchoReq, EchoRes> {
  constructor(private readonly evidence: EvidenceStore) {}

  async handle(request: EchoReq, context: ZLinkMessageContext): Promise<EchoRes> {
    this.evidence.add(`codec-request|codec=json|value=${request.value}|content=${context.contentType}`);
    return { value: `echo:${request.value}`, contentType: context.contentType ?? '<null>' };
  }
}

@Injectable()
export class JsonEchoCommandHandler implements ZLinkSendHandler<EchoMsg> {
  constructor(private readonly evidence: EvidenceStore) {}

  async handle(message: EchoMsg, context: ZLinkMessageContext): Promise<void> {
    this.evidence.add(`codec-command|codec=json|id=${message.commandId}|value=${message.value}|content=${context.contentType}`);
  }
}

@Injectable()
export class ProtobufEchoRequestHandler implements ZLinkRequestHandler<EchoReq, ProtobufEchoReq> {
  constructor(private readonly evidence: EvidenceStore) {}

  async handle(request: EchoReq, context: ZLinkMessageContext): Promise<ProtobufEchoReq> {
    this.evidence.add(`codec-request|codec=protobuf|value=${request.value}|content=${context.contentType}`);
    return new ProtobufEchoReq(`echo:${request.value}|content:${context.contentType ?? '<null>'}`);
  }
}

@Injectable()
export class ProtobufEchoCommandHandler implements ZLinkSendHandler<ProtobufEchoMsg> {
  constructor(private readonly evidence: EvidenceStore) {}

  async handle(message: ProtobufEchoMsg, context: ZLinkMessageContext): Promise<void> {
    this.evidence.add(`codec-command|codec=protobuf|value=${message.value}|content=${context.contentType}`);
  }
}

@Injectable()
export class MessagePackEchoRequestHandler implements ZLinkRequestHandler<EchoReq, MessagePackEchoReq> {
  constructor(private readonly evidence: EvidenceStore) {}

  async handle(request: EchoReq, context: ZLinkMessageContext): Promise<MessagePackEchoReq> {
    this.evidence.add(`codec-request|codec=msgpack|value=${request.value}|content=${context.contentType}`);
    return new MessagePackEchoReq(`echo:${request.value}|content:${context.contentType ?? '<null>'}`);
  }
}

@Injectable()
export class MessagePackEchoCommandHandler implements ZLinkSendHandler<MessagePackEchoMsg> {
  constructor(private readonly evidence: EvidenceStore) {}

  async handle(message: MessagePackEchoMsg, context: ZLinkMessageContext): Promise<void> {
    this.evidence.add(`codec-command|codec=msgpack|id=${message.commandId}|value=${message.value}|content=${context.contentType}`);
  }
}
