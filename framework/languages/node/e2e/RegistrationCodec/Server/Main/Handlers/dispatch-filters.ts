import { Injectable } from '@nestjs/common';
import type { ZLinkHandlerFilterNext, ZLinkHandlerFilter, ZLinkHandlerFilterContext } from '@zlink-systems/framework';
import { EvidenceStore } from '../Infrastructure/evidence-store';

@Injectable()
export class FirstFilter implements ZLinkHandlerFilter {
  constructor(private readonly evidence: EvidenceStore) {}

  async invoke(context: ZLinkHandlerFilterContext, next: ZLinkHandlerFilterNext): Promise<void> {
    this.evidence.add(`filter|name=first|phase=before|packet=${context.packetName ?? '<null>'}`);
    await next();
    this.evidence.add(`filter|name=first|phase=after|packet=${context.packetName ?? '<null>'}`);
  }
}

@Injectable()
export class SecondFilter implements ZLinkHandlerFilter {
  constructor(private readonly evidence: EvidenceStore) {}

  async invoke(context: ZLinkHandlerFilterContext, next: ZLinkHandlerFilterNext): Promise<void> {
    this.evidence.add(`filter|name=second|phase=before|packet=${context.packetName ?? '<null>'}`);
    await next();
    this.evidence.add(`filter|name=second|phase=after|packet=${context.packetName ?? '<null>'}`);
  }
}
