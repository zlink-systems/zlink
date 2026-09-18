import { Injectable } from '@nestjs/common';
import type { ZLinkMessageContext, ZLinkRequestHandler } from '@zlink-systems/framework';
import type { GetPlayerProfile, PlayerProfile } from '../../Shared/contracts';

// --8<-- [start:channel-request-handler]
// Answers a request addressed to the "profile" channel. Any node that exposes
// this channel may receive it; the caller does not pick one.
@Injectable()
export class GetPlayerProfileHandler
implements ZLinkRequestHandler<GetPlayerProfile, PlayerProfile> {
  async handle(request: GetPlayerProfile, context: ZLinkMessageContext): Promise<PlayerProfile> {
    void context;
    return { playerId: request.playerId, nickname: 'rookie', level: 1 };
  }
}
// --8<-- [end:channel-request-handler]
