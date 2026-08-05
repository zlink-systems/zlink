export {
  ConversationStatuses,
  SupportChatRoles
} from '../../../../Shared/Contracts/messages';

export type {
  ChatMessage,
  ConversationState,
  ConversationStatus,
  SupportRole
} from '../../../../Shared/Contracts/messages';

type ConversationParticipant = {
  actorId: string;
  role: import('../../../../Shared/Contracts/messages').SupportRole;
  displayName: string;
  joinedAtUnixMs: number;
  isTyping: boolean;
  lastTypingChangedAtUnixMs?: number;
};

export type { ConversationParticipant };
