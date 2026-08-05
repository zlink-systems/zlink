import type {
  ZLinkMessage,
  ZLinkSession,
  ZLinkSessionContext,
  ZLinkSessionDispatchContext
} from '@zlink-systems/framework';

// --8<-- [start:doc-session]
class PlaySession implements ZLinkSession {
  constructor(readonly context: ZLinkSessionContext) {}

  async onDispatch(dispatch: ZLinkSessionDispatchContext, payload: ZLinkMessage): Promise<void> {
    if (await this.context.handlers.tryHandle(dispatch, payload)) return;
    const actor = this.context.actors.bound.length === 1 ? this.context.actors.bound[0] : undefined;
    if (actor === undefined) throw new Error(`AuthenticateReq is required before '${dispatch.packetName}'.`);
    await actor.relay(payload);
  }

  async onDisconnected(): Promise<void> {
    await Promise.allSettled(this.context.actors.bound.map((actor) => actor.notifyDisconnected()));
  }
}
// --8<-- [end:doc-session]

export { PlaySession };
