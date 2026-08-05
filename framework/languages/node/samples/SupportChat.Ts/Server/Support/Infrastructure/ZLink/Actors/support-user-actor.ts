import {
  zlinkEntrySpotActorRequestHandler,
  zlinkEntrySpotActorSendHandler,
  zlinkSpotActorSendHandler
} from '@zlink-systems/nestjs';
import { SupportEntrySpot } from '../Spots/EntrySpot/support-entry-spot';
import { ConversationSpot } from '../Spots/ConversationSpot/conversation-spot';
import { joinConversation } from '../../../../../Shared/Contracts/messages';
import {
  ChatMessageNotify,
  ConversationAssignedNotify,
  ConversationClosedNotify,
  ConversationIdleNotify,
  ParticipantJoinedNotify,
  TypingChangedNotify
} from '../../../../../Shared/Contracts/messages';
import type { ConversationState } from '../../../../../Shared/Contracts/messages';
import type { SupportRole } from '../../../../../Shared/Contracts/messages';
import type {
  ZLinkActor,
  ZLinkActorContext,
  ZLinkActorJoinCompletion,
  ZLinkMessageContext
} from '@zlink-systems/framework';
import {
  JoinConversationFailedNotify
} from '../../../../../Shared/Contracts/messages';

class DeliverSupportNotification {
  readonly packetName: string;

  constructor(readonly message: unknown, readonly conversationId: string) {
    this.packetName = typeof message === 'object' && message !== null
      ? message.constructor.name
      : '';
  }
}

class JoinSupportConversation {
  constructor(
    readonly conversationId: string,
    readonly participantId: string,
    readonly role: SupportRole,
    readonly displayName: string
  ) {}
}

class SupportUserActor implements ZLinkActor {
  private pendingConversationId?: string;
  private readonly completedJoinOperations = new Set<string>();

  constructor(readonly actorId: string, readonly context: ZLinkActorContext) {}

  async push(message: unknown, conversationId: string): Promise<void> {
    try {
      await this.context.boundSession
        .send(message)
        .metadata('conversation-id', conversationId)
        .submit();
    } catch {
      // Conversation state remains in the Spot and is returned after reconnect.
    }
  }

  scheduleConversationJoin(message: JoinSupportConversation): {
    readonly scheduled: true;
    readonly state: ConversationState;
  } {
    if (this.pendingConversationId !== undefined) {
      throw new Error('A conversation join is already pending.');
    }
    this.pendingConversationId = message.conversationId;
    this.context.joinSpot(
      message.conversationId,
      joinConversation(message.participantId, message.role, message.displayName)
    ).defer();
    return {
      scheduled: true,
      state: {
        conversationId: message.conversationId,
        subject: '',
        status: 'WaitingForAgent',
        customerActorId: message.role === 'Customer' ? message.participantId : '',
        lastMessageSeq: 0
      }
    };
  }

  async onJoinCompleted(completion: ZLinkActorJoinCompletion): Promise<void> {
    const operationKey = `${completion.operationId.high}:${completion.operationId.low}`;
    if (this.completedJoinOperations.has(operationKey)) {
      return;
    }
    this.completedJoinOperations.add(operationKey);

    const conversationId = this.pendingConversationId;
    if (conversationId === undefined) {
      return;
    }
    this.pendingConversationId = undefined;
    if (completion.status === 'accepted') {
      return;
    }
    if (completion.status === 'rejected') {
      await this.push(
        new JoinConversationFailedNotify(conversationId, 'Rejected', false),
        conversationId
      );
      return;
    }
    await this.push(
      new JoinConversationFailedNotify(
        conversationId,
        String(completion.kind),
        false
      ),
      conversationId
    );
  }
}

@zlinkEntrySpotActorSendHandler({
  entrySpot: () => SupportEntrySpot,
  actor: () => SupportUserActor,
  packetName: 'DeliverSupportNotification'
})
@zlinkSpotActorSendHandler({
  spot: () => ConversationSpot,
  actor: () => SupportUserActor,
  packetName: 'DeliverSupportNotification'
})
class DeliverSupportNotificationHandler {
  async handle(_spot: unknown, actor: SupportUserActor, _context: ZLinkMessageContext, message: DeliverSupportNotification): Promise<void> {
    await actor.push(
      rehydrateSupportNotification(message.packetName, message.message),
      message.conversationId
    );
  }
}

function rehydrateSupportNotification(packetName: string, payload: unknown): unknown {
  const value = payload as Record<string, unknown>;
  switch (packetName) {
    case 'ParticipantJoinedNotify':
      return new ParticipantJoinedNotify(
        value.conversationId as string,
        value.actorId as string,
        value.role as SupportRole,
        value.state as ConversationState
      );
    case 'ConversationAssignedNotify':
      return new ConversationAssignedNotify(value.conversationId as string, value.state as ConversationState);
    case 'ChatMessageNotify':
      return new ChatMessageNotify(
        value.conversationId as string,
        value.message as never,
        value.state as ConversationState
      );
    case 'TypingChangedNotify':
      return new TypingChangedNotify(
        value.conversationId as string,
        value.actorId as string,
        value.isTyping as boolean,
        value.state as ConversationState
      );
    case 'ConversationIdleNotify':
      return new ConversationIdleNotify(value.conversationId as string, value.state as ConversationState);
    case 'ConversationClosedNotify':
      return new ConversationClosedNotify(value.conversationId as string, value.state as ConversationState);
    default:
      throw new Error(`Unsupported SupportChat notification '${packetName}'.`);
  }
}

@zlinkEntrySpotActorRequestHandler({
  entrySpot: () => SupportEntrySpot,
  actor: () => SupportUserActor,
  packetName: 'JoinSupportConversation'
})
class JoinSupportConversationHandler {
  async handle(
    _spot: SupportEntrySpot,
    actor: SupportUserActor,
    _context: ZLinkMessageContext,
    message: JoinSupportConversation
  ): Promise<{ readonly scheduled: true; readonly state: ConversationState }> {
    return actor.scheduleConversationJoin(message);
  }
}

export {
  DeliverSupportNotification,
  DeliverSupportNotificationHandler,
  JoinSupportConversation,
  JoinSupportConversationHandler,
  SupportUserActor
};
