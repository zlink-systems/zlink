package systems.zlink.samples.supportchat.server.support.infrastructure;

import systems.zlink.samples.supportchat.server.configuration.SampleNames;
import systems.zlink.samples.supportchat.server.support.domain.Conversation;
import systems.zlink.samples.supportchat.shared.contracts.Messages;

public final class ConversationContracts {
    private ConversationContracts() {
    }

    public static Messages.ConversationState state(Conversation.Snapshot value) {
        return new Messages.ConversationState(
            value.conversationId(), value.subject(), status(value.status()),
            value.customerActorId(), value.agentActorId(), value.lastMessageSeq(),
            value.lastMessageAtUnixMs(), value.idleDeadlineUnixMs());
    }

    public static Messages.ChatMessage message(Conversation.Message value) {
        return new Messages.ChatMessage(
            value.conversationId(), value.messageSeq(), value.senderActorId(),
            value.text(), value.sentAtUnixMs());
    }

    public static String role(Conversation.Role value) {
        return value == Conversation.Role.Customer
            ? SampleNames.Roles.Customer : SampleNames.Roles.Agent;
    }

    private static String status(Conversation.Status value) {
        return switch (value) {
            case WaitingForAgent -> SampleNames.Statuses.WaitingForAgent;
            case Active -> SampleNames.Statuses.Active;
            case WaitingForClose -> SampleNames.Statuses.WaitingForClose;
            case Closed -> SampleNames.Statuses.Closed;
        };
    }
}
