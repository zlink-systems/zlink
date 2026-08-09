import { Inject, Injectable } from '@nestjs/common';
import { ZLINK_ACTOR_CLIENT, ZLINK_ACTOR_MANAGER } from '@zlink-systems/nestjs';
import { ZLinkPacket } from '@zlink-systems/framework';
import {
  ActorLocationProbeReq,
  ActorLocationProbeRes,
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
class MessageFollowProbeSessionHandler {
  constructor(@Inject(ZLINK_ACTOR_CLIENT) private readonly actors: ZLinkActorClient) {}

  async handle(
    context: ZLinkSessionContext,
    dispatch: ZLinkSessionDispatchContext,
    payload: ZLinkMessage
  ): Promise<void> {
    const request = payload.decode(MessageFollowProbeReq);
    const probe = new MessageFollowProbeReq(request.actorId, request.probeId, request.payload);
    try {
      if (dispatch.canReply) {
        const reply = await this.actors
          .requestToActor(request.actorId, probe)
          .timeout(10_000)
          .submit<MessageFollowProbeRes>();
        context.client.reply(new MessageFollowProbeRes(reply.probeId, reply.payload)).submit();
        return;
      }
      await this.actors.sendToActor(request.actorId, probe).submit();
    } catch (error) {
      console.error(
        `message-follow probe terminal actor=${request.actorId} probe=${request.probeId}`,
        error instanceof Error ? error.message : String(error)
      );
      if (dispatch.canReply) {
        context.client.reply(
          new MessageFollowProbeRes(request.probeId, '', ZoneWorldErrors.actorUnavailable)
        ).submit();
      }
    }
  }
}

export { ActorLocationProbeHandler, MessageFollowProbeSessionHandler };
