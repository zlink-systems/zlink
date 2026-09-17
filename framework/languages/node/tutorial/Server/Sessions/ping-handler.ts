import { Injectable } from '@nestjs/common';
import { ZLinkPacket } from '@zlink-systems/framework';
import type {
  ZLinkMessage,
  ZLinkSessionContext,
  ZLinkSessionDispatchContext
} from '@zlink-systems/framework';
import { PacketNames, Ping, Pong } from '../../Shared/contracts';

// --8<-- [start:session-handler]
// The packet name comes from the decorator, and it has to match what the client
// sends. The first argument is the session context, not the session.
@Injectable()
@ZLinkPacket(PacketNames.ping)
class PingHandler {
  async handle(
    context: ZLinkSessionContext,
    _dispatch: ZLinkSessionDispatchContext,
    payload: ZLinkMessage
  ): Promise<void> {
    const message = payload.decode(Ping);

    // reply answers a request. To push to a client that is not waiting for one,
    // use client.send instead.
    await context.client.reply(new Pong(message.sentAtUnixMs)).submit();
  }
}
// --8<-- [end:session-handler]

export { PingHandler };
