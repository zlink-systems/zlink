import { Inject, Injectable } from '@nestjs/common';
import { ZLINK_ACTOR_CLIENT, ZLINK_ACTOR_MANAGER } from '@zlink-systems/nestjs';
import { ZLinkPacket } from '@zlink-systems/framework';
import {
  ActorLocationProbeReq,
  ActorLocationProbeRes,
  MessageFollowProbeMsg,
  MessageFollowProbeReq,
  MessageFollowProbeRes,
  PacketNames
} from '../../Shared/contracts';
import { ZoneWorldErrors } from '../../Shared/spec';
import type {
  ZLinkActorClient,
  ZLinkActorManager,
  ZLinkMessage,
  ZLinkSessionContext,
  ZLinkSessionDispatchContext
} from '@zlink-systems/framework';

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
  constructor(@Inject(ZLINK_ACTOR_CLIENT) private readonly actors: ZLinkActorClient) {}

  async handle(
    context: ZLinkSessionContext,
    _dispatch: ZLinkSessionDispatchContext,
    payload: ZLinkMessage
  ): Promise<void> {
    const request = payload.decode(MessageFollowProbeReq);
    try {
      const reply = await this.actors
        .requestToActor(
          request.actorId,
          new MessageFollowProbeReq(request.actorId, request.probeId, request.payload)
        )
        .timeout(10_000)
        .submit<MessageFollowProbeRes>();
      context.client.reply(new MessageFollowProbeRes(reply.probeId, reply.payload)).submit();
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
  constructor(@Inject(ZLINK_ACTOR_CLIENT) private readonly actors: ZLinkActorClient) {}

  async handle(
    _context: ZLinkSessionContext,
    _dispatch: ZLinkSessionDispatchContext,
    payload: ZLinkMessage
  ): Promise<void> {
    const message = payload.decode(MessageFollowProbeMsg);
    try {
      await this.actors.sendToActor(
        message.actorId,
        new MessageFollowProbeMsg(message.actorId, message.probeId, message.payload)
      ).submit();
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
  MessageFollowProbeRequestSessionHandler,
  MessageFollowProbeSendSessionHandler
};
