import {
  zlinkSpotActorRequestHandler,
  zlinkSpotActorSendHandler
} from '@zlink-systems/nestjs';
import { SampleNames } from '../../../../../../Configuration/sample-names';
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

abstract class ConversationActorRoute {
  protected assertMembership(actor: SupportUserActor, context: ZLinkMessageContext): void {
    const conversationId = context.metadata.find(SampleNames.conversationIdMetadataKey);
    if (conversationId === undefined || String(actor.context.spotId) !== conversationId) {
      throw new Error('Conversation metadata does not match the actor membership.');
    }
  }
}

@zlinkSpotActorRequestHandler({
  actor: () => SupportUserActor,
  spot: () => ConversationSpot,
  packetName: PacketNames.joinConversationReq
})
class JoinConversationHandler extends ConversationActorRoute
  implements ZLinkSpotActorRequestHandler<ConversationSpot, SupportUserActor, JoinConversationReq, JoinConversationRes> {
  async handle(
    spot: ConversationSpot,
    actor: SupportUserActor,
    context: ZLinkMessageContext
  ): Promise<JoinConversationRes> {
    this.assertMembership(actor, context);
    return { scheduled: false, state: spot.join(actor.actorId) };
  }
}

@zlinkSpotActorRequestHandler({
  actor: () => SupportUserActor,
  spot: () => ConversationSpot,
  packetName: PacketNames.sendChatMessageReq
})
class SendChatMessageHandler extends ConversationActorRoute
  implements ZLinkSpotActorRequestHandler<ConversationSpot, SupportUserActor, SendChatMessageReq, SendChatMessageRes> {
  async handle(
    spot: ConversationSpot,
    actor: SupportUserActor,
    context: ZLinkMessageContext,
    request: SendChatMessageReq
  ): Promise<SendChatMessageRes> {
    this.assertMembership(actor, context);
    return await spot.sendChat(actor.actorId, request.text);
  }
}

@zlinkSpotActorSendHandler({
  actor: () => SupportUserActor,
  spot: () => ConversationSpot,
  packetName: PacketNames.setTypingMsg
})
class SetTypingHandler extends ConversationActorRoute
  implements ZLinkSpotActorSendHandler<ConversationSpot, SupportUserActor, SetTypingMsg> {
  async handle(
    spot: ConversationSpot,
    actor: SupportUserActor,
    context: ZLinkMessageContext,
    request: SetTypingMsg
  ): Promise<void> {
    this.assertMembership(actor, context);
    await spot.setTyping(actor.actorId, request.isTyping);
  }
}

@zlinkSpotActorRequestHandler({
  actor: () => SupportUserActor,
  spot: () => ConversationSpot,
  packetName: PacketNames.closeConversationReq
})
class CloseConversationHandler extends ConversationActorRoute
  implements ZLinkSpotActorRequestHandler<ConversationSpot, SupportUserActor, CloseConversationReq, CloseConversationRes> {
  constructor(private readonly availability: AgentAvailabilityDirectory) {
    super();
  }

  async handle(
    spot: ConversationSpot,
    actor: SupportUserActor,
    context: ZLinkMessageContext
  ): Promise<CloseConversationRes> {
    this.assertMembership(actor, context);
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
