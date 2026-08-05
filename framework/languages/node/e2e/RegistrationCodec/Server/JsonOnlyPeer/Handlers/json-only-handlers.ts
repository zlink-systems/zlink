import { Injectable } from '@nestjs/common';
import type { ZLinkMessageContext, ZLinkRequestHandler } from '@zlink-systems/framework';
import { type EchoRes, type EchoReq } from '../../../Shared/messages';
import { EvidenceStore } from '../Infrastructure/evidence-store';

@Injectable()
export class JsonOnlyEchoRequestHandler implements ZLinkRequestHandler<EchoReq, EchoRes> {
  constructor(private readonly evidence: EvidenceStore) {}

  async handle(request: EchoReq, context: ZLinkMessageContext): Promise<EchoRes> {
    if (context.contentType !== 'application/json') {
      this.evidence.add(`codec-mismatch-rejected|content=${context.contentType ?? '<null>'}`);
      throw new Error(`JSON-only peer rejected content type '${context.contentType ?? '<null>'}'.`);
    }
    this.evidence.add(`codec-mismatch-json|value=${request.value}|content=${context.contentType}`);
    return { value: `echo:${request.value}`, contentType: context.contentType ?? '<null>' };
  }
}
