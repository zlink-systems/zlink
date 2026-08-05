type SupportRole = 'Agent' | 'Customer';
type ConversationStatus = 'WaitingForAgent' | 'Active' | 'WaitingForClose' | 'Closed';

class AuthenticateReq { constructor(readonly accessToken: string) {} }
class AuthenticateRes {
  constructor(readonly actorId: string, readonly displayName: string, readonly role: SupportRole) {}
}
class AuthenticateUserReq { constructor(readonly accessToken: string) {} }
type AuthenticateUserRes = {
  accepted: boolean;
  actorId?: string;
  displayName?: string;
  role?: SupportRole;
  reason?: string;
};
class SupportUserActorCreateReq {
  constructor(
    readonly actorId: string,
    readonly displayName: string,
    readonly role: SupportRole,
    readonly participantId: string
  ) {}
}
class OpenConversationApiReq {
  constructor(readonly customerActorId: string, readonly customerDisplayName: string, readonly subject: string) {}
}
type OpenConversationApiRes = { conversationId: string; status: ConversationStatus };
class OpenConversationReq { constructor(readonly subject: string) {} }
type OpenConversationRes = { conversationId: string; state: ConversationState };
class SetAgentAvailableReq { constructor(readonly isAvailable: boolean) {} }
type SetAgentAvailableRes = { isAvailable: boolean };
class JoinConversationReq {
  constructor(
    readonly participantId: string,
    readonly role: SupportRole,
    readonly displayName: string
  ) {}
}
class JoinConversationRes {
  constructor(
    readonly scheduled: boolean,
    readonly state: ConversationState
  ) {}
}
class JoinConversationFailedNotify {
  constructor(
    readonly conversationId: string,
    readonly error: string,
    readonly isRetriable: boolean
  ) {}
}
class SendChatMessageReq { constructor(readonly text: string) {} }
type SendChatMessageRes = { message: ChatMessage; state: ConversationState };
class SetTypingMsg { constructor(readonly isTyping: boolean) {} }
class CloseConversationReq { constructor(readonly reason?: string) {} }
type CloseConversationRes = { state: ConversationState };
class ParticipantJoinedNotify { constructor(readonly conversationId: string, readonly actorId: string, readonly role: SupportRole, readonly state: ConversationState) {} }
class ConversationAssignedNotify { constructor(readonly conversationId: string, readonly state: ConversationState) {} }
class ChatMessageNotify { constructor(readonly conversationId: string, readonly message: ChatMessage, readonly state: ConversationState) {} }
class TypingChangedNotify { constructor(readonly conversationId: string, readonly actorId: string, readonly isTyping: boolean, readonly state: ConversationState) {} }
class ConversationIdleNotify { constructor(readonly conversationId: string, readonly state: ConversationState) {} }
class ConversationClosedNotify { constructor(readonly conversationId: string, readonly state: ConversationState) {} }
type ConversationState = {
  conversationId: string;
  subject: string;
  status: ConversationStatus;
  customerActorId: string;
  agentActorId?: string;
  lastMessageSeq: number;
  lastMessageAtUnixMs?: number;
  idleDeadlineUnixMs?: number;
};
type ChatMessage = {
  conversationId: string;
  messageSeq: number;
  senderActorId: string;
  text: string;
  sentAtUnixMs: number;
};

const SupportChatRoles = { Agent: 'Agent', Customer: 'Customer' } as const;
const ConversationStatuses = {
  WaitingForAgent: 'WaitingForAgent',
  Active: 'Active',
  WaitingForClose: 'WaitingForClose',
  Closed: 'Closed'
} as const;

const PacketNames = {
  authenticateReq: 'AuthenticateReq',
  authenticateUserReq: 'AuthenticateUserReq',
  openConversationApiReq: 'OpenConversationApiReq',
  openConversationReq: 'OpenConversationReq',
  setAgentAvailableReq: 'SetAgentAvailableReq',
  joinConversationReq: 'JoinConversationReq',
  joinConversationFailedNotify: 'JoinConversationFailedNotify',
  sendChatMessageReq: 'SendChatMessageReq',
  setTypingMsg: 'SetTypingMsg',
  closeConversationReq: 'CloseConversationReq',
  participantJoinedNotify: 'ParticipantJoinedNotify',
  conversationAssignedNotify: 'ConversationAssignedNotify',
  chatMessageNotify: 'ChatMessageNotify',
  typingChangedNotify: 'TypingChangedNotify',
  conversationIdleNotify: 'ConversationIdleNotify',
  conversationClosedNotify: 'ConversationClosedNotify'
} as const;

const authenticate = (accessToken: string) => new AuthenticateReq(accessToken);
const authenticateUser = (accessToken: string) => new AuthenticateUserReq(accessToken);
const openConversationApi = (customerActorId: string, customerDisplayName: string, subject: string) =>
  new OpenConversationApiReq(customerActorId, customerDisplayName, subject);
const openConversation = (subject: string) => new OpenConversationReq(subject);
const setAgentAvailable = (isAvailable: boolean) => new SetAgentAvailableReq(isAvailable);
const joinConversation = (participantId = '', role: SupportRole = SupportChatRoles.Customer, displayName = '') =>
  new JoinConversationReq(participantId, role, displayName);
const sendChatMessage = (text: string) => new SendChatMessageReq(text);
const setTyping = (isTyping: boolean) => new SetTypingMsg(isTyping);
const closeConversation = (reason?: string) => new CloseConversationReq(reason);

export {
  PacketNames,
  AuthenticateReq,
  AuthenticateRes,
  AuthenticateUserReq,
  SupportUserActorCreateReq,
  OpenConversationApiReq,
  OpenConversationReq,
  SetAgentAvailableReq,
  JoinConversationReq,
  JoinConversationRes,
  JoinConversationFailedNotify,
  SendChatMessageReq,
  SetTypingMsg,
  CloseConversationReq,
  ParticipantJoinedNotify,
  ConversationAssignedNotify,
  ChatMessageNotify,
  TypingChangedNotify,
  ConversationIdleNotify,
  ConversationClosedNotify,
  SupportChatRoles,
  ConversationStatuses,
  authenticate,
  authenticateUser,
  openConversationApi,
  openConversation,
  setAgentAvailable,
  joinConversation,
  sendChatMessage,
  setTyping,
  closeConversation
};

export type {
  SupportRole,
  ConversationStatus,
  AuthenticateUserRes,
  OpenConversationApiRes,
  OpenConversationRes,
  SetAgentAvailableRes,
  SendChatMessageRes,
  CloseConversationRes,
  ConversationState,
  ChatMessage
};
