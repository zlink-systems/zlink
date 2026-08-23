import { Inject, Injectable } from '@nestjs/common';
import { ZLINK_ACTOR_MANAGER } from '@zlink-systems/nestjs';
import { ZLinkPacket } from '@zlink-systems/framework';
import {
  ActorLocationProbeReq,
  ActorLocationProbeRes,
  CreateFreshActorProbeReq,
  CreateFreshActorProbeRes,
  MessageFollowProbeMsg,
  MessageFollowProbeReq,
  MessageFollowProbeRes,
  PacketNames,
  PlayerActorCreateReq
} from '../../Shared/contracts';
import { ZoneWorldErrors, ZoneWorldNames } from '../../Shared/spec';
import type {
  ZLinkActorManager,
  ZLinkMessage,
  ZLinkSessionContext,
  ZLinkSessionDispatchContext
} from '@zlink-systems/framework';

/**
 * Performs the explicit new-object operation used by ZW-G3/G4. Capacity
 * placement selects an owner; the returned NodeRid is evidence only and is
 * never supplied as placement input.
 */
@Injectable()
@ZLinkPacket(PacketNames.createFreshActorProbeReq)
class CreateFreshActorProbeHandler {
  constructor(@Inject(ZLINK_ACTOR_MANAGER) private readonly actors: ZLinkActorManager) {}

  async handle(
    context: ZLinkSessionContext,
    _dispatch: ZLinkSessionDispatchContext,
    payload: ZLinkMessage
  ): Promise<void> {
    const request = payload.decode(CreateFreshActorProbeReq);
    const result = await this.actors
      .getOrCreate(request.actorId, ZoneWorldNames.playerActorType)
      .inMesh(ZoneWorldNames.zoneMesh)
      .request(new PlayerActorCreateReq(request.actorId))
      .submit();
    context.client.reply(result.status === 'rejected'
      ? new CreateFreshActorProbeRes(request.actorId, '', '', ZoneWorldErrors.actorUnavailable)
      : new CreateFreshActorProbeRes(
        result.actor.actorId,
        result.actor.objectGeneration.toString(),
        String(result.actor.nodeRid)
      )).submit();
  }
}

/**
 * Serves runner-only relocation probes through the Gateway's existing Object
 * Client. ObjectGeneration and NodeRid values are evidence only and are never
 * used for application routing.
 */
@Injectable()
@ZLinkPacket(PacketNames.actorLocationProbeReq)
class ActorLocationProbeHandler {
  constructor(@Inject(ZLINK_ACTOR_MANAGER) private readonly actors: ZLinkActorManager) {}

  async handle(
    context: ZLinkSessionContext,
    _dispatch: ZLinkSessionDispatchContext,
    payload: ZLinkMessage
  ): Promise<void> {
    const request = payload.decode(ActorLocationProbeReq);
    const actor = await this.actors.find(request.actorId);
    const response = actor === undefined
      ? new ActorLocationProbeRes(request.actorId, '', '', ZoneWorldErrors.actorNotFound)
      : new ActorLocationProbeRes(actor.actorId, actor.objectGeneration.toString(), String(actor.nodeRid));
    context.client.reply(response).submit();
  }
}

/**
 * Forwards a probe through the Gateway's public Actor client. Right after a
 * relocation, the bounded route cache makes the call enter the previous owner,
 * where Message Follow must deliver it to the committed target without
 * application retry or route reconstruction (ZW-B6). When there is no route at
 * all, the call ends with a terminal error instead of following anywhere.
 */
@Injectable()
@ZLinkPacket(PacketNames.messageFollowProbeReq)
class MessageFollowProbeRequestSessionHandler {
  async handle(
    context: ZLinkSessionContext,
    _dispatch: ZLinkSessionDispatchContext,
    payload: ZLinkMessage
  ): Promise<void> {
    const request = payload.decode(MessageFollowProbeReq);
    const actor = context.actors.bound.find((candidate) => candidate.actorId === request.actorId);
    if (actor === undefined) {
      context.client.reply(
        new MessageFollowProbeRes(request.probeId, '', ZoneWorldErrors.actorUnavailable)
      ).submit();
      return;
    }
    try {
      await actor.relay(payload);
    } catch (error) {
      console.error(
        `message-follow probe terminal actor=${request.actorId} probe=${request.probeId}`,
        error instanceof Error ? error.message : String(error)
      );
      context.client.reply(
        new MessageFollowProbeRes(request.probeId, '', ZoneWorldErrors.actorUnavailable)
      ).submit();
    }
  }
}

@Injectable()
@ZLinkPacket(PacketNames.messageFollowProbeMsg)
class MessageFollowProbeSendSessionHandler {
  async handle(
    context: ZLinkSessionContext,
    _dispatch: ZLinkSessionDispatchContext,
    payload: ZLinkMessage
  ): Promise<void> {
    const message = payload.decode(MessageFollowProbeMsg);
    const actor = context.actors.bound.find((candidate) => candidate.actorId === message.actorId);
    if (actor === undefined) return;
    try {
      await actor.relay(payload);
    } catch (error) {
      console.error(
        `message-follow probe terminal actor=${message.actorId} probe=${message.probeId}`,
        error instanceof Error ? error.message : String(error)
      );
    }
  }
}

export {
  ActorLocationProbeHandler,
  CreateFreshActorProbeHandler,
  MessageFollowProbeRequestSessionHandler,
  MessageFollowProbeSendSessionHandler
};
