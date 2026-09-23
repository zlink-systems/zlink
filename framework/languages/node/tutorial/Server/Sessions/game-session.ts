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
    // A packet with an Actor slot selects its binding. A packet without one
    // can use the connection only when exactly one Actor is bound.
    const bound = this.context.actors.bound;
    const actor = dispatch.actor ?? (bound.length === 1 ? bound[0] : undefined);
    if (!actor) {
      throw new Error(
        bound.length === 0
          ? 'Authenticate an Actor before sending player packets.'
          : 'Select an Actor handle when more than one Actor is bound.'
      );
    }

    await actor.relay(dispatch, payload);
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
