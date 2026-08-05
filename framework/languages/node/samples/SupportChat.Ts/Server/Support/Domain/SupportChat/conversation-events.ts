import type { ChatMessage, ConversationState, SupportRole } from './conversation-models';

type ConversationEvent =
  | { readonly kind: 'assigned'; readonly actorId: string; readonly state: ConversationState }
  | { readonly kind: 'participantJoined'; readonly actorId: string; readonly role: SupportRole; readonly state: ConversationState }
  | { readonly kind: 'messageAppended'; readonly message: ChatMessage; readonly state: ConversationState }
  | { readonly kind: 'typingChanged'; readonly actorId: string; readonly isTyping: boolean; readonly state: ConversationState }
  | { readonly kind: 'idle'; readonly state: ConversationState }
  | { readonly kind: 'closed'; readonly state: ConversationState };

export type { ConversationEvent };
