import type {
  ZLinkMessage,
  ZLinkSession,
  ZLinkSessionContext,
  ZLinkSessionDispatchContext,
  ZLinkSessionFactory
} from '@zlink-systems/framework';

class BingoSession implements ZLinkSession {
  constructor(
    readonly context: ZLinkSessionContext
  ) {}

  async onDispatch(dispatch: ZLinkSessionDispatchContext, payload: ZLinkMessage, signal?: AbortSignal): Promise<void> {
    if (await this.context.handlers.tryHandle(dispatch, payload)) {
      return;
    }
    const actor = this.context.actors.bound.length === 1 ? this.context.actors.bound[0] : undefined;
    if (actor === undefined) {
      throw new Error(`Client must authenticate before relaying packet '${dispatch.packetName}'.`);
    }
    await actor.relay(payload, signal);
  }

  async onDisconnected(): Promise<void> {
    const actors = this.context.actors.bound;
    await Promise.allSettled(actors.map((actor) => actor.notifyDisconnected()));
    console.error(`bingo-lifecycle session-disconnect actor=${actors[0]?.actorId ?? '-'} destroy=false`);
  }
}

class BingoSessionFactory implements ZLinkSessionFactory<BingoSession> {
  async create(context: ZLinkSessionContext): Promise<BingoSession> {
    return new BingoSession(context);
  }
}

export { BingoSession, BingoSessionFactory };
