import { Injectable, Scope } from '@nestjs/common';
import { SampleTimings } from '../../../../../Configuration/sample-names';
import { SupportChatRoles } from '../../../../../../Shared/Contracts/messages';
import { Conversation } from '../../../../Domain/SupportChat/conversation';
import { ConversationIdleTimerHandler } from './Handlers/conversation-idle-timer-handler';
import { SupportNotificationPublisher } from './Notifications/support-notification-publisher';
import { AgentAssignmentService } from '../../../../Application/ConversationAssignment/agent-assignment-service';
import { SupportActorDirectory } from '../../Actors/support-actor-directory';
import type {
  ZLinkMessage,
  ZLinkSpot,
  ZLinkSpotActorJoinResult,
  ZLinkSpotContext,
  ZLinkSpotCreateResponse,
  ZLinkTimer
} from '@zlink-systems/framework';
import type {
  ChatMessage,
  ConversationState,
  JoinConversationReq,
  SupportRole
} from '../../../../../../Shared/Contracts/messages';
import type { ConversationCreateRequest } from './conversation-create-request';
import type { ConversationEvent } from '../../../../Domain/SupportChat/conversation-events';
import type { SupportUserActor } from '../../Actors/support-user-actor';

interface ConversationJoinIntent {
  readonly actorId: string;
  readonly participantId: string;
  readonly role: SupportRole;
  readonly displayName: string;
}

interface ConversationParticipant extends ConversationJoinIntent {}

@Injectable({ scope: Scope.TRANSIENT })
class ConversationSpot implements ZLinkSpot<SupportUserActor> {
  readonly context!: ZLinkSpotContext<SupportUserActor, ConversationSpot>;
  private conversation?: Conversation;
  private readonly actors = new Map<string, ConversationParticipant>();
  private readonly pendingJoins = new Map<string, ConversationJoinIntent>();
  private timer?: ZLinkTimer;

  constructor(
    private readonly assignments: AgentAssignmentService,
    private readonly directory: SupportActorDirectory,
    private readonly notifications: SupportNotificationPublisher
  ) {}

  async onCreate(request: ZLinkMessage): Promise<ZLinkSpotCreateResponse> {
    const value = request.decode<ConversationCreateRequest>(Object as never);
    this.conversation = new Conversation(
      String(this.context.spotId),
      value.customerActorId,
      value.customerDisplayName,
      value.subject
    );
    return { accepted: true };
  }

  async onInitialize(): Promise<void> {
    this.timer = await this.context.addTimer('conversation-idle', 50, ConversationIdleTimerHandler);
  }

  async onClosing(): Promise<void> {
    await this.timer?.cancel();
  }

  async onActorJoin(
    actorId: string,
    request: ZLinkMessage
  ): Promise<ZLinkSpotActorJoinResult> {
    const join = request.decode<JoinConversationReq>(Object as never);
    this.pendingJoins.set(actorId, {
      actorId,
      participantId: join.participantId,
      role: join.role,
      displayName: join.displayName
    });
    return { accepted: true, reply: { state: this.snapshot() } };
  }

  async onJoinedActor(actor: SupportUserActor): Promise<void> {
    const intent = this.pendingJoins.get(actor.actorId);
    if (intent === undefined) return;
    this.pendingJoins.delete(actor.actorId);
    const participant: ConversationParticipant = { ...intent };
    this.actors.set(actor.actorId, participant);
    if (participant.role === SupportChatRoles.Agent) {
      const joined = this.requireConversation().join(
        participant.participantId,
        SupportChatRoles.Agent,
        participant.displayName
      );
      const customer = this.findParticipant(joined.state.customerActorId);
      await this.notifications.publish(
        joined.event,
        customer === undefined ? [actor.actorId] : [customer.actorId, actor.actorId]
      );
      return;
    }
    const agentActorId = this.assignments.assignNextAgent();
    if (agentActorId === undefined) return;
    const roster = this.directory.get(agentActorId);
    if (roster === undefined) {
      throw new Error(`Assigned roster actor '${agentActorId}' was not found.`);
    }
    const assigned = this.assignAgent(agentActorId, roster.displayName);
    await this.notifications.publish(assigned.event, [roster.actorId]);
  }

  async onLeaveActor(actor: SupportUserActor): Promise<void> {
    this.actors.delete(actor.actorId);
  }

  async onDisconnectActor(_actor: SupportUserActor): Promise<void> {}

  snapshot(): ConversationState {
    return this.requireConversation().snapshot();
  }

  assignAgent(agentActorId: string, displayName: string): { state: ConversationState; event: ConversationEvent } {
    return this.requireConversation().assign(agentActorId, displayName);
  }

  join(actorId: string): ConversationState {
    const actor = this.requireActor(actorId);
    return this.requireConversation().join(actor.participantId, actor.role, actor.displayName).state;
  }

  async sendChat(actorId: string, text: string): Promise<{ message: ChatMessage; state: ConversationState }> {
    const actor = this.requireActor(actorId);
    this.requireParticipant(actor);
    const result = this.requireConversation().appendMessage(actor.participantId, text);
    await this.notifications.publish(result.event, this.otherActorRefs(actor.actorId));
    return result;
  }

  async setTyping(actorId: string, isTyping: boolean): Promise<void> {
    const actor = this.requireActor(actorId);
    this.requireParticipant(actor);
    const event = this.requireConversation().changeTyping(actor.participantId, isTyping);
    if (event !== undefined) await this.notifications.publish(event, this.otherActorRefs(actor.actorId));
  }

  async close(actorId: string): Promise<ConversationState> {
    const actor = this.requireActor(actorId);
    this.requireParticipant(actor);
    const closed = this.requireConversation().close();
    await this.notifications.publish(closed.event, this.otherActorRefs(actor.actorId));
    return closed.state;
  }

  async onTimer(now = Date.now()): Promise<string | undefined> {
    const conversation = this.requireConversation();
    if (conversation.shouldBecomeIdle(now, SampleTimings.idleTimeout)) {
      const idle = conversation.markIdle(now + SampleTimings.closeGraceTimeout);
      if (idle.event !== undefined) await this.notifications.publish(idle.event, this.actorRefs());
      return undefined;
    }
    if (conversation.shouldCloseAfterIdle(now)) {
      const closed = conversation.close();
      await this.notifications.publish(closed.event, this.actorRefs());
      await this.context.close();
      return closed.state.agentActorId;
    }
    return undefined;
  }

  private requireParticipant(actor: ConversationParticipant): void {
    if (!this.requireConversation().canParticipate(actor.participantId)) {
      throw new Error(`Actor '${actor.participantId}' is not a conversation participant.`);
    }
  }

  private otherActorRefs(sourceActorId: string): string[] {
    return [...this.actors.values()]
      .filter((actor) => actor.actorId !== sourceActorId)
      .map((actor) => actor.actorId);
  }

  private actorRefs(): string[] {
    return [...this.actors.values()].map((actor) => actor.actorId);
  }

  private findParticipant(participantId: string): ConversationParticipant | undefined {
    return [...this.actors.values()].find((actor) => actor.participantId === participantId);
  }

  private requireActor(actorId: string): ConversationParticipant {
    const actor = this.actors.get(actorId);
    if (actor === undefined) throw new Error(`Actor '${actorId}' is not joined to this conversation.`);
    return actor;
  }

  private requireConversation(): Conversation {
    if (this.conversation === undefined) {
      throw new Error('Conversation Spot has not been created.');
    }
    return this.conversation;
  }
}

export { ConversationSpot };
