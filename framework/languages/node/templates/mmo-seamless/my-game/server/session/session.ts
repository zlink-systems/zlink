import { Inject, Injectable } from '@nestjs/common';
import { ZLINK_ACTOR_MANAGER } from '@zlink-systems/nestjs';
import type {
  ZLinkActorManager,
  ZLinkMessage,
  ZLinkSession,
  ZLinkSessionContext,
  ZLinkSessionDispatchContext,
  ZLinkSessionFactory
} from '@zlink-systems/framework';
import { Game, Names, PacketNames } from '../shared/constants';
import { ActionRejected, JoinWorldRequest, SessionInfo } from '../shared/messages';
export class Session implements ZLinkSession {
  constructor(
    readonly context: ZLinkSessionContext,
    private readonly actors: ZLinkActorManager
  ) {}
  async onDispatch(dispatch: ZLinkSessionDispatchContext, payload: ZLinkMessage): Promise<void> {
    const allowed = new Set<string>([
      PacketNames.joinWorldRequest,
      PacketNames.moveRequest,
      PacketNames.openDungeonRequest,
      PacketNames.sessionInfoRequest
    ]);
    if (!allowed.has(dispatch.packetName)) {
      await this.context.client.send(new ActionRejected('packet-not-allowed', 0)).submit();
      return;
    }
    if (!this.context.actors.bound.length) {
      if (dispatch.packetName !== PacketNames.joinWorldRequest)
        throw new Error('JoinWorldRequest must be first');
      const request = payload.decode(JoinWorldRequest);
      const ensured = await this.actors
        .getOrCreate(request.playerId, Names.actor)
        .inMesh(Names.mesh)
        .submit();
      if (ensured.status === 'rejected') throw new Error('Actor creation rejected');
      await this.context.actors.bindOrGet(ensured.actor);
    }
    const actor = this.context.actors.bound[0];
    if (!actor) throw new Error('JoinWorldRequest must be first');
    if (dispatch.packetName === PacketNames.sessionInfoRequest) {
      if (actor.ref.objectGeneration > BigInt(Game.maxSafeInteger)) {
        await this.context.client.reply(new ActionRejected('integer-out-of-range', 0)).submit();
        return;
      }
      await this.context.client
        .reply(
          new SessionInfo(
            String(this.context.sessionId),
            actor.ref.actorId,
            actor.ref.objectGeneration
          )
        )
        .submit();
      return;
    }
    await actor.relay(payload);
  }
}
@Injectable()
export class SessionFactory implements ZLinkSessionFactory<Session> {
  constructor(@Inject(ZLINK_ACTOR_MANAGER) private readonly actors: ZLinkActorManager) {}
  async create(context: ZLinkSessionContext): Promise<Session> {
    return new Session(context, this.actors);
  }
}
