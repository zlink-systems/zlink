import { Injectable } from '@nestjs/common';
import { zlinkSpotPacketHandler } from '@zlink-systems/nestjs';
import type {
  ZLinkMessageContext,
  ZLinkSpotPacketHandler,
  ZLinkSpotRequestHandler
} from '@zlink-systems/framework';
import type {
  CloseConversationRes,
  JoinConversationRes,
  SendChatMessageRes
} from '../../../../../../../Shared/Contracts/messages';
import { ConversationSpot } from '../conversation-spot';

class JoinConversationAtSpotReq { constructor(readonly actorId: string) {} }
class SendChatMessageAtSpotReq { constructor(readonly actorId: string, readonly text: string) {} }
class SetTypingAtSpotMsg { constructor(readonly actorId: string, readonly isTyping: boolean) {} }
class CloseConversationAtSpotReq { constructor(readonly actorId: string) {} }

@Injectable()
@zlinkSpotPacketHandler({ spot: () => ConversationSpot, packetName: 'JoinConversationAtSpotReq' })
class JoinConversationAtSpotHandler
  implements ZLinkSpotRequestHandler<ConversationSpot, JoinConversationAtSpotReq, JoinConversationRes> {
  async handle(spot: ConversationSpot, request: JoinConversationAtSpotReq, _context: ZLinkMessageContext): Promise<JoinConversationRes> {
    return { scheduled: false, state: spot.join(request.actorId) };
  }
}

@Injectable()
@zlinkSpotPacketHandler({ spot: () => ConversationSpot, packetName: 'SendChatMessageAtSpotReq' })
class SendChatMessageAtSpotHandler
  implements ZLinkSpotRequestHandler<ConversationSpot, SendChatMessageAtSpotReq, SendChatMessageRes> {
  async handle(spot: ConversationSpot, request: SendChatMessageAtSpotReq, _context: ZLinkMessageContext): Promise<SendChatMessageRes> {
    return await spot.sendChat(request.actorId, request.text);
  }
}

@Injectable()
@zlinkSpotPacketHandler({ spot: () => ConversationSpot, packetName: 'SetTypingAtSpotMsg' })
class SetTypingAtSpotHandler implements ZLinkSpotPacketHandler<ConversationSpot, SetTypingAtSpotMsg> {
  async handle(spot: ConversationSpot, message: SetTypingAtSpotMsg, _context: ZLinkMessageContext): Promise<void> {
    await spot.setTyping(message.actorId, message.isTyping);
  }
}

@Injectable()
@zlinkSpotPacketHandler({ spot: () => ConversationSpot, packetName: 'CloseConversationAtSpotReq' })
class CloseConversationAtSpotHandler
  implements ZLinkSpotRequestHandler<ConversationSpot, CloseConversationAtSpotReq, CloseConversationRes> {
  async handle(spot: ConversationSpot, request: CloseConversationAtSpotReq, _context: ZLinkMessageContext): Promise<CloseConversationRes> {
    return { state: await spot.close(request.actorId) };
  }
}

export {
  CloseConversationAtSpotHandler,
  CloseConversationAtSpotReq,
  JoinConversationAtSpotHandler,
  JoinConversationAtSpotReq,
  SendChatMessageAtSpotHandler,
  SendChatMessageAtSpotReq,
  SetTypingAtSpotHandler,
  SetTypingAtSpotMsg
};
