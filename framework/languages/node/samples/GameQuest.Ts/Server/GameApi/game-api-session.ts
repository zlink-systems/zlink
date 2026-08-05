import { Inject, Injectable } from '@nestjs/common';
import { ZLINK_ACTOR_MANAGER, ZLINK_SPOT_OUTBOUND } from '@zlink-systems/nestjs';
import { questMissionSpotId, SampleNames } from '../../Shared/Configuration/sample-names';
import {
  JoinSessionRes,
  PacketNames,
  getQuestProgressReq
} from '../../Shared/Contracts/messages';
import {
  ZLinkPacket,
  type ZLinkActorManager,
  type ZLinkMessage,
  type ZLinkSpotOutbound,
  type ZLinkSession,
  type ZLinkSessionContext,
  type ZLinkSessionDispatchContext,
  type ZLinkSessionFactory
} from '@zlink-systems/framework';
import type {
  JoinSessionReq
} from '../../Shared/Contracts/messages';
import type { GetQuestProgressRes } from '../../Shared/Contracts/messages';

class GameQuestSession implements ZLinkSession {
  constructor(readonly context: ZLinkSessionContext) {}

  async onDispatch(dispatch: ZLinkSessionDispatchContext, payload: ZLinkMessage): Promise<void> {
    if (await this.context.handlers.tryHandle(dispatch, payload)) return;
    const actor = this.context.actors.bound.length === 1 ? this.context.actors.bound[0] : undefined;
    if (actor === undefined) throw new Error(`JoinSessionReq is required before '${dispatch.packetName}'.`);
    await actor.relay(payload);
  }
}

@Injectable()
@ZLinkPacket(PacketNames.joinSessionReq)
class JoinSessionHandler {
  constructor(
    @Inject(ZLINK_SPOT_OUTBOUND) private readonly spots: ZLinkSpotOutbound,
    @Inject(ZLINK_ACTOR_MANAGER) private readonly actorManager: ZLinkActorManager
  ) {}

  async handle(
    context: ZLinkSessionContext,
    _dispatch: ZLinkSessionDispatchContext,
    payload: ZLinkMessage
  ): Promise<void> {
    const request = payload.decode<JoinSessionReq>(Object as never);
    const existing = context.actors.bound.at(0);
    if (existing !== undefined && existing.actorId !== request.playerId) {
      throw new Error(`Session is already bound to player '${existing.actorId}'.`);
    }
    const located = await this.actorManager.find(request.playerId);
    const created = located === undefined
      ? await this.actorManager
        .getOrCreate(request.playerId, SampleNames.playerActorType)
        .inMesh(SampleNames.playerQuestSpotMesh)
        .request(request)
        .submit()
      : undefined;
    if (created?.status === 'rejected') {
      throw new Error(`Player actor '${request.playerId}' creation was rejected.`);
    }
    const actorRef = located ?? created?.actor;
    if (actorRef === undefined) {
      throw new Error(`Player actor '${request.playerId}' was not resolved.`);
    }
    await context.actors.bindOrGet(actorRef);
    const current = await this.getProjection(request.playerId);
    context.client.reply(new JoinSessionRes(request.playerId, current.activeQuests)).submit();
  }

  private async getProjection(playerId: string): Promise<GetQuestProgressRes> {
    return await this.spots
      .requestToSpot(questMissionSpotId(playerId), getQuestProgressReq(playerId))
      .instanceSpot(SampleNames.playerQuestSpotType)
      .inMesh(SampleNames.playerQuestSpotMesh)
      .timeout(SampleNames.requestTimeout)
      .submit<GetQuestProgressRes>();
  }
}

class GameQuestSessionFactory implements ZLinkSessionFactory<GameQuestSession> {
  async create(context: ZLinkSessionContext): Promise<GameQuestSession> {
    return new GameQuestSession(context);
  }
}

export { GameQuestSession, GameQuestSessionFactory, JoinSessionHandler };
