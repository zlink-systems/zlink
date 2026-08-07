import { Injectable } from '@nestjs/common';
import type { ZLinkMessageContext, ZLinkRequestHandler } from '@zlink-systems/framework';
import {
  ClientServerReq,
  type ClientServerRes,
  SecondaryReq,
  type SecondaryRes
} from '../../../Shared/messages';
import { EvidenceStore } from '../Infrastructure/evidence-store';

@Injectable()
export class SecondaryRequestHandler implements ZLinkRequestHandler<SecondaryReq, SecondaryRes> {
  constructor(private readonly evidence: EvidenceStore) {}

  async handle(request: SecondaryReq, _context: ZLinkMessageContext): Promise<SecondaryRes> {
    this.evidence.add(`secondary-request|rid=${this.evidence.rid}|value=${request.value}|marker=${request.marker ?? ''}`);
    return { value: `secondary:${request.value}`, providerRid: this.evidence.rid, marker: request.marker };
  }
}

@Injectable()
export class ClientServerRequestHandler implements ZLinkRequestHandler<ClientServerReq, ClientServerRes> {
  constructor(private readonly evidence: EvidenceStore) {}

  async handle(request: ClientServerReq, _context: ZLinkMessageContext): Promise<ClientServerRes> {
    this.evidence.add(`client-server-request|rid=${this.evidence.rid}|value=${request.value}|marker=${request.marker ?? ''}`);
    return { value: `client-server:${request.value}`, providerRid: this.evidence.rid, marker: request.marker };
  }
}
