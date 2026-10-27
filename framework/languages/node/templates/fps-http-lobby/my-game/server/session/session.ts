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
import { Names, PacketNames as GamePacketNames, Rejection } from '../shared/constants';
import { PacketNames as LobbyPacketNames } from '../shared/lobby-constants';
import { ActionRejected, SessionInfo } from '../shared/messages';
import { JoinRoomRequest } from '../shared/lobby-messages';

const AllowedClientPackets = new Set<string>([
  LobbyPacketNames.joinRoomRequest,
  GamePacketNames.roomViewRequest,
  GamePacketNames.moveRequest,
  GamePacketNames.shootRequest,
  GamePacketNames.sessionInfoRequest
]);

export class Session implements ZLinkSession {
  constructor(
    readonly context: ZLinkSessionContext,
    private readonly actors: ZLinkActorManager
  ) {}
  async onDispatch(dispatch: ZLinkSessionDispatchContext, payload: ZLinkMessage): Promise<void> {
    if (!AllowedClientPackets.has(dispatch.packetName)) {
      await this.context.client.send(new ActionRejected(Rejection.packetNotAllowed)).submit();
      return;
    }
    if (!this.context.actors.bound.length) {
      if (dispatch.packetName !== LobbyPacketNames.joinRoomRequest) {
        await this.context.client.send(new ActionRejected(Rejection.packetNotAllowed)).submit();
        return;
      }
      const request = payload.decode(JoinRoomRequest);
      const ensured = await this.actors
        .getOrCreate(request.playerId, Names.actor)
        .inMesh(Names.mesh)
        .submit();
      if (ensured.status === 'rejected') throw new Error('Actor creation rejected');
      await this.context.actors.bindOrGet(ensured.actor);
    }
    const actor = this.context.actors.bound[0];
    if (!actor) throw new Error('JoinRoomRequest must be first');
    if (dispatch.packetName === GamePacketNames.sessionInfoRequest) {
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
