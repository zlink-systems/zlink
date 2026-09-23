import { zlinkSpotActorRequestHandler, zlinkSpotActorSendHandler } from '@zlink-systems/nestjs';
import { AgentAvailabilityDirectory } from '../../../../../Application/ConversationAssignment/agent-availability-directory';
import { PacketNames } from '../../../../../../../Shared/Contracts/messages';
import { SupportUserActor } from '../../../Actors/support-user-actor';
import { ConversationSpot } from '../conversation-spot';
import type {
  CloseConversationReq,
  CloseConversationRes,
  JoinConversationReq,
  JoinConversationRes,
  SendChatMessageReq,
  SendChatMessageRes,
  SetTypingMsg
} from '../../../../../../../Shared/Contracts/messages';
import type {
  ZLinkMessageContext,
  ZLinkSpotActorRequestHandler,
  ZLinkSpotActorSendHandler
} from '@zlink-systems/framework';

@zlinkSpotActorRequestHandler({
  actor: () => SupportUserActor,
  spot: () => ConversationSpot,
  packetName: PacketNames.joinConversationReq
})
class JoinConversationHandler implements ZLinkSpotActorRequestHandler<
  ConversationSpot,
  SupportUserActor,
  JoinConversationReq,
  JoinConversationRes
> {
  async handle(
    spot: ConversationSpot,
    actor: SupportUserActor,
    _context: ZLinkMessageContext,
    request: JoinConversationReq
  ): Promise<JoinConversationRes> {
    if (request.conversationId !== String(actor.context.spotId))
      throw new Error('JoinConversationReq does not match the actor membership.');
    return { scheduled: false, actorId: actor.actorId, state: spot.join(actor.actorId) };
  }
}

@zlinkSpotActorRequestHandler({
  actor: () => SupportUserActor,
  spot: () => ConversationSpot,
  packetName: PacketNames.sendChatMessageReq
})
class SendChatMessageHandler implements ZLinkSpotActorRequestHandler<
  ConversationSpot,
  SupportUserActor,
  SendChatMessageReq,
  SendChatMessageRes
> {
  async handle(
    spot: ConversationSpot,
    actor: SupportUserActor,
    _context: ZLinkMessageContext,
    request: SendChatMessageReq
  ): Promise<SendChatMessageRes> {
    return await spot.sendChat(actor.actorId, request.text);
  }
}

@zlinkSpotActorSendHandler({
  actor: () => SupportUserActor,
  spot: () => ConversationSpot,
  packetName: PacketNames.setTypingMsg
})
class SetTypingHandler implements ZLinkSpotActorSendHandler<
  ConversationSpot,
  SupportUserActor,
  SetTypingMsg
> {
  async handle(
    spot: ConversationSpot,
    actor: SupportUserActor,
    _context: ZLinkMessageContext,
    request: SetTypingMsg
  ): Promise<void> {
    await spot.setTyping(actor.actorId, request.isTyping);
  }
}

@zlinkSpotActorRequestHandler({
  actor: () => SupportUserActor,
  spot: () => ConversationSpot,
  packetName: PacketNames.closeConversationReq
})
class CloseConversationHandler implements ZLinkSpotActorRequestHandler<
  ConversationSpot,
  SupportUserActor,
  CloseConversationReq,
  CloseConversationRes
> {
  constructor(private readonly availability: AgentAvailabilityDirectory) {}

  async handle(
    spot: ConversationSpot,
    actor: SupportUserActor,
    _context: ZLinkMessageContext
  ): Promise<CloseConversationRes> {
    const response = { state: await spot.close(actor.actorId) };
    if (response.state.agentActorId !== undefined) {
      this.availability.released(response.state.agentActorId);
    }
    return response;
  }
}

export {
  JoinConversationHandler,
  SendChatMessageHandler,
  SetTypingHandler,
  CloseConversationHandler
};
