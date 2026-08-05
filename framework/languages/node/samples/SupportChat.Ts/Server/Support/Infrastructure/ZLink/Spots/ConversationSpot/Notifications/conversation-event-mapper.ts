import {
  ChatMessageNotify,
  ConversationAssignedNotify,
  ConversationClosedNotify,
  ConversationIdleNotify,
  ParticipantJoinedNotify,
  TypingChangedNotify
} from '../../../../../../../Shared/Contracts/messages';
import type { ConversationEvent } from '../../../../../Domain/SupportChat/conversation-events';

class ConversationEventMapper {
  map(event: ConversationEvent): unknown {
    switch (event.kind) {
      case 'assigned': return new ConversationAssignedNotify(event.state.conversationId, event.state);
      case 'participantJoined': return new ParticipantJoinedNotify(event.state.conversationId, event.actorId, event.role, event.state);
      case 'messageAppended': return new ChatMessageNotify(event.state.conversationId, event.message, event.state);
      case 'typingChanged': return new TypingChangedNotify(event.state.conversationId, event.actorId, event.isTyping, event.state);
      case 'idle': return new ConversationIdleNotify(event.state.conversationId, event.state);
      case 'closed': return new ConversationClosedNotify(event.state.conversationId, event.state);
    }
  }
}

export { ConversationEventMapper };
