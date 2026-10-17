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
import { Names, PacketNames, meshName } from '../shared/constants';
import { NODE_CONFIG, type NodeConfig } from '../shared/config';
import { EnterWorldRequest, SessionInfo } from '../shared/messages';
export class Session implements ZLinkSession {
  constructor(
    readonly context: ZLinkSessionContext,
    private readonly actors: ZLinkActorManager,
    private readonly config: NodeConfig
  ) {}
  async onDispatch(dispatch: ZLinkSessionDispatchContext, payload: ZLinkMessage): Promise<void> {
    if (!this.context.actors.bound.length) {
      if (dispatch.packetName !== PacketNames.enterWorldRequest)
        throw new Error('EnterWorldRequest must be first');
      const request = payload.decode(EnterWorldRequest);
      if (!request.playerId) throw new Error('Player ID required');
      const ensured = await this.actors
        .getOrCreate(request.playerId, Names.actor)
        .inMesh(meshName(this.config.worldId))
        .submit();
      if (ensured.status === 'rejected') throw new Error('Actor creation rejected');
      await this.context.actors.bindOrGet(ensured.actor);
    }
    const actor = this.context.actors.bound[0];
    if (!actor) throw new Error('EnterWorldRequest must be first');
    if (dispatch.packetName === PacketNames.sessionInfoRequest) {
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
  constructor(
    @Inject(ZLINK_ACTOR_MANAGER) private readonly actors: ZLinkActorManager,
    @Inject(NODE_CONFIG) private readonly config: NodeConfig
  ) {}
  async create(context: ZLinkSessionContext): Promise<Session> {
    return new Session(context, this.actors, this.config);
  }
}
