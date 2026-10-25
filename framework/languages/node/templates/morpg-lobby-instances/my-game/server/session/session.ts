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
import { Names, PacketNames, Rejection } from '../shared/constants';
import { JoinLobbyRequest, SessionInfo, ActionRejected } from '../shared/messages';
const clientPackets: readonly string[] = [PacketNames.joinLobbyRequest, PacketNames.createPartyRequest,
  PacketNames.joinPartyRequest, PacketNames.readyRequest, PacketNames.enterDungeonRequest,
  PacketNames.advanceRequest, PacketNames.viewRequest, PacketNames.sessionInfoRequest];
export class Session implements ZLinkSession {
  constructor(
    readonly context: ZLinkSessionContext,
    private readonly actors: ZLinkActorManager
  ) {}
  async onDispatch(dispatch: ZLinkSessionDispatchContext, payload: ZLinkMessage): Promise<void> {
    if (!clientPackets.includes(dispatch.packetName)) {
      await this.context.client.send(new ActionRejected(Rejection.packetNotAllowed)).submit(); return;
    }
    if (!this.context.actors.bound.length) {
      if (dispatch.packetName !== PacketNames.joinLobbyRequest)
        throw new Error('JoinLobbyRequest must be first');
      const request = payload.decode(JoinLobbyRequest);
      const ensured = await this.actors
        .getOrCreate(request.playerId, Names.actor)
        .inMesh(Names.mesh)
        .submit();
      if (ensured.status === 'rejected') throw new Error('Actor creation rejected');
      await this.context.actors.bindOrGet(ensured.actor);
    }
    const actor = this.context.actors.bound[0];
    if (!actor) throw new Error('JoinLobbyRequest must be first');
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
  constructor(@Inject(ZLINK_ACTOR_MANAGER) private readonly actors: ZLinkActorManager) {}
  async create(context: ZLinkSessionContext): Promise<Session> {
    return new Session(context, this.actors);
  }
}
