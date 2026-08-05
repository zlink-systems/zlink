import { ConversationStatuses } from './conversation-models';
import { ConversationPolicy } from './conversation-policy';
import type { ConversationEvent } from './conversation-events';
import type { ChatMessage, ConversationParticipant, ConversationState, SupportRole } from './conversation-models';

class Conversation {
  private readonly messages: ChatMessage[] = [];
  private readonly participants = new Map<string, ConversationParticipant>();
  private state: ConversationState;

  constructor(
    conversationId: string,
    customerActorId: string,
    customerDisplayName: string,
    subject: string,
    now = Date.now(),
    private readonly policy = new ConversationPolicy()
  ) {
    this.state = {
      conversationId,
      subject,
      status: ConversationStatuses.WaitingForAgent,
      customerActorId,
      lastMessageSeq: 0,
      lastMessageAtUnixMs: now
    };
    this.participants.set(customerActorId, {
      actorId: customerActorId,
      role: 'Customer',
      displayName: customerDisplayName,
      joinedAtUnixMs: now,
      isTyping: false
    });
  }

  snapshot(): ConversationState {
    return { ...this.state };
  }

  assign(agentActorId: string, displayName: string, now = Date.now()): { state: ConversationState; event: ConversationEvent } {
    if (this.state.status === ConversationStatuses.Closed) {
      throw new Error('Closed conversation cannot be assigned.');
    }
    if (this.state.agentActorId !== undefined && this.state.agentActorId !== agentActorId) {
      throw new Error('Conversation already has a different assigned agent.');
    }
    if (!this.participants.has(agentActorId) && this.participants.size >= this.policy.maximumParticipants) {
      throw new Error(`Conversation supports at most ${this.policy.maximumParticipants} participants.`);
    }
    this.state = { ...this.state, agentActorId };
    if (!this.participants.has(agentActorId)) {
      this.participants.set(agentActorId, {
        actorId: agentActorId,
        role: 'Agent',
        displayName,
        joinedAtUnixMs: now,
        isTyping: false
      });
    }
    const state = this.snapshot();
    return { state, event: { kind: 'assigned', actorId: agentActorId, state } };
  }

  join(actorId: string, role: SupportRole, displayName: string): { state: ConversationState; event: ConversationEvent } {
    this.requireParticipant(actorId);
    const participant = this.participants.get(actorId)!;
    if (participant.role !== role || participant.displayName !== displayName) {
      throw new Error(`Actor '${actorId}' identity does not match the conversation participant.`);
    }
    if (this.state.status === ConversationStatuses.Closed) {
      const state = this.snapshot();
      return { state, event: { kind: 'participantJoined', actorId, role, state } };
    }
    if (
      actorId === this.state.agentActorId
      && this.state.status === ConversationStatuses.WaitingForAgent
    ) {
      this.state = {
        ...this.state,
        status: ConversationStatuses.Active,
        lastMessageAtUnixMs: Date.now(),
        idleDeadlineUnixMs: undefined
      };
    }
    const state = this.snapshot();
    return { state, event: { kind: 'participantJoined', actorId, role, state } };
  }

  appendMessage(senderActorId: string, text: string): { message: ChatMessage; state: ConversationState; event: ConversationEvent } {
    this.requireParticipant(senderActorId);
    if (this.state.status === ConversationStatuses.Closed) {
      throw new Error('Closed conversation must reject messages.');
    }
    this.policy.validateMessage(text);
    const now = Date.now();
    const message: ChatMessage = {
      conversationId: this.state.conversationId,
      messageSeq: this.state.lastMessageSeq + 1,
      senderActorId,
      text,
      sentAtUnixMs: now
    };
    this.messages.push(message);
    this.state = {
      ...this.state,
      status: ConversationStatuses.Active,
      lastMessageSeq: message.messageSeq,
      lastMessageAtUnixMs: now,
      idleDeadlineUnixMs: undefined
    };
    const state = this.snapshot();
    return { message, state, event: { kind: 'messageAppended', message, state } };
  }

  changeTyping(actorId: string, isTyping: boolean): ConversationEvent | undefined {
    if (!this.canParticipate(actorId) || this.state.status === ConversationStatuses.Closed) return undefined;
    const participant = this.participants.get(actorId)!;
    this.participants.set(actorId, { ...participant, isTyping, lastTypingChangedAtUnixMs: Date.now() });
    return { kind: 'typingChanged', actorId, isTyping, state: this.snapshot() };
  }

  markIdle(deadlineUnixMs: number): { state: ConversationState; event?: ConversationEvent } {
    if (this.state.status !== ConversationStatuses.Active) {
      return { state: this.snapshot() };
    }
    this.state = {
      ...this.state,
      status: ConversationStatuses.WaitingForClose,
      idleDeadlineUnixMs: deadlineUnixMs
    };
    const state = this.snapshot();
    return { state, event: { kind: 'idle', state } };
  }

  close(): { state: ConversationState; event: ConversationEvent } {
    if (this.state.status === ConversationStatuses.Closed) {
      throw new Error('Closed conversation must reject a duplicate close.');
    }
    this.state = {
      ...this.state,
      status: ConversationStatuses.Closed,
      idleDeadlineUnixMs: undefined
    };
    const state = this.snapshot();
    return { state, event: { kind: 'closed', state } };
  }

  canParticipate(actorId: string): boolean {
    return actorId === this.state.customerActorId || actorId === this.state.agentActorId;
  }

  shouldBecomeIdle(now: number, idleTimeoutMs: number): boolean {
    return this.state.status === ConversationStatuses.Active
      && now - (this.state.lastMessageAtUnixMs ?? now) >= idleTimeoutMs;
  }

  shouldCloseAfterIdle(now: number): boolean {
    return this.state.status === ConversationStatuses.WaitingForClose
      && this.state.idleDeadlineUnixMs !== undefined
      && now >= this.state.idleDeadlineUnixMs;
  }

  private requireParticipant(actorId: string): void {
    if (!this.canParticipate(actorId)) {
      throw new Error(`Actor '${actorId}' is not a conversation participant.`);
    }
  }
}

export { Conversation };
