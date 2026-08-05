import { Injectable } from '@nestjs/common';
import type { ZLinkRequestHandler } from '@zlink-systems/framework';
import { type EchoRes, type EchoReq } from '../../../Shared/messages';

@Injectable()
export class DuplicateEchoRequestHandler implements ZLinkRequestHandler<EchoReq, EchoRes> {
  async handle(request: EchoReq): Promise<EchoRes> {
    return { value: request.value, contentType: 'duplicate' };
  }
}
