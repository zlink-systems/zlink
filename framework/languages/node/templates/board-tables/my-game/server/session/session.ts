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
import { Names, PacketNames, Reasons } from '../shared/constants';
import { ActionRejected, FindMatch, JoinTableRequest, SessionInfo } from '../shared/messages';

const clientPackets = new Set<string>([
  PacketNames.joinTableRequest,
  PacketNames.findMatch,
  PacketNames.cancelMatch,
  PacketNames.tableViewRequest,
  PacketNames.startHandRequest,
  PacketNames.betRequest,
  PacketNames.sessionInfoRequest
]);

export class Session implements ZLinkSession {
  constructor(
    readonly context: ZLinkSessionContext,
    private readonly actors: ZLinkActorManager
  ) {}
  async onDispatch(dispatch: ZLinkSessionDispatchContext, payload: ZLinkMessage): Promise<void> {
    if (!clientPackets.has(dispatch.packetName)) {
      await this.context.client.send(new ActionRejected(Reasons.packetNotAllowed, 0)).submit();
      return;
    }
    let actor = this.context.actors.bound[0];
    if (dispatch.packetName === PacketNames.joinTableRequest && actor)
      throw new Error('JoinTableRequest must be first');
    if (!actor) {
      const playerId =
        dispatch.packetName === PacketNames.joinTableRequest
          ? payload.decode(JoinTableRequest).playerId
          : dispatch.packetName === PacketNames.findMatch
            ? payload.decode(FindMatch).playerId
            : undefined;
      if (!playerId)
        throw new Error('JoinTableRequest or FindMatch with a player ID must be first');
      const ensured = await this.actors
        .getOrCreate(playerId, Names.actor)
        .inMesh(Names.mesh)
        .submit();
      if (ensured.status === 'rejected') throw new Error('Actor creation rejected');
      actor = await this.context.actors.bindOrGet(ensured.actor);
    }
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
