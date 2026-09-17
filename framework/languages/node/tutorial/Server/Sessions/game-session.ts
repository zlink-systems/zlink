import { Injectable } from '@nestjs/common';
import type {
  ZLinkMessage,
  ZLinkSession,
  ZLinkSessionContext,
  ZLinkSessionDispatchContext,
  ZLinkSessionFactory
} from '@zlink-systems/framework';
import { AuthenticateHandler } from './authenticate-handler';
import { PingHandler } from './ping-handler';

// --8<-- [start:session-class]
// One connected game client. Callbacks for the same connection run in order.
class GameSession implements ZLinkSession {
  constructor(readonly context: ZLinkSessionContext) {}

  async onConnected(context: ZLinkSessionContext): Promise<void> {
    console.log(`client connected: ${context.sessionId}`);
  }

  async onDisconnected(context: ZLinkSessionContext): Promise<void> {
    console.log(`client disconnected: ${context.sessionId}`);
  }

  // Every inbound packet arrives here first. Registering a handler is not
  // enough on its own; this method is what routes the packet to it.
  async onDispatch(dispatch: ZLinkSessionDispatchContext, payload: ZLinkMessage): Promise<void> {
    if (await this.context.handlers.tryHandle(dispatch, payload)) return;

    // --8<-- [start:session-actor-relay]
    // Anything without a session handler is forwarded to the player bound to
    // this connection, which is why authentication has to come first.
    const bound = this.context.actors.bound;
    if (bound.length !== 1) {
      throw new Error('Authenticate before sending player packets.');
    }

    await bound[0].relay(payload);
    // --8<-- [end:session-actor-relay]
  }
}

// Session handlers are not discovered by scanning. The factory is where they
// are registered, and without them no packet is ever handled.
@Injectable()
class GameSessionFactory implements ZLinkSessionFactory<GameSession> {
  async create(context: ZLinkSessionContext): Promise<GameSession> {
    context.handlers.addHandler(PingHandler);
    context.handlers.addHandler(AuthenticateHandler);

    return new GameSession(context);
  }
}
// --8<-- [end:session-class]

export { GameSession, GameSessionFactory };
