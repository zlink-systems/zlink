import { Inject, Injectable } from '@nestjs/common';
import { ZLINK_ACTOR_MANAGER } from '@zlink-systems/nestjs';
import { ZLinkPacket } from '@zlink-systems/framework';
import { PacketNames, PlayerActorCreateReq } from '../../Shared/contracts';
import { ZoneWorldNames } from '../../Shared/spec';
import type { JoinWorldReq } from '../../Shared/contracts';
import type {
  ZLinkActorManager,
  ZLinkMessage,
  ZLinkSession,
  ZLinkSessionContext,
  ZLinkSessionDispatchContext,
  ZLinkSessionFactory
} from '@zlink-systems/framework';

class PlayerSession implements ZLinkSession {
  constructor(readonly context: ZLinkSessionContext) {}

  async onDispatch(dispatch: ZLinkSessionDispatchContext, payload: ZLinkMessage): Promise<void> {
    if (await this.context.handlers.tryHandle(dispatch, payload)) return;
    const actor = this.context.actors.bound.length === 1 ? this.context.actors.bound[0] : undefined;
    if (actor === undefined) throw new Error(`Client must join before sending '${dispatch.packetName}'.`);
    await actor.relay(payload);
  }

  async onDisconnected(): Promise<void> {
    await Promise.allSettled(
      this.context.actors.bound.map((actor) => actor.notifyDisconnected(AbortSignal.timeout(3_000)))
    );
  }
}

@Injectable()
@ZLinkPacket(PacketNames.joinWorldReq)
class JoinWorldSessionHandler {
  constructor(@Inject(ZLINK_ACTOR_MANAGER) private readonly actors: ZLinkActorManager) {}

  async handle(
    context: ZLinkSessionContext,
    _dispatch: ZLinkSessionDispatchContext,
    payload: ZLinkMessage
  ): Promise<void> {
    if (context.actors.bound.length !== 0) throw new Error('Session already joined the world.');
    const request = payload.decode<JoinWorldReq>(Object as never);
    const ensured = await this.actors
      .getOrCreate(request.playerId, ZoneWorldNames.playerActorType)
      .inMesh(ZoneWorldNames.zoneMesh)
      .request(new PlayerActorCreateReq(request.playerId))
      .timeout(5_000)
      .submit();
    if (ensured.status === 'rejected') throw new Error(`Player actor '${request.playerId}' creation was rejected.`);
    const actor = await context.actors.bindOrGet(ensured.actor);
    await actor.relay(payload);
    console.log(`session bound to player actor player=${request.playerId}`);
  }
}

class PlayerSessionFactory implements ZLinkSessionFactory<PlayerSession> {
  async create(context: ZLinkSessionContext): Promise<PlayerSession> {
    return new PlayerSession(context);
  }
}

export { JoinWorldSessionHandler, PlayerSession, PlayerSessionFactory };
