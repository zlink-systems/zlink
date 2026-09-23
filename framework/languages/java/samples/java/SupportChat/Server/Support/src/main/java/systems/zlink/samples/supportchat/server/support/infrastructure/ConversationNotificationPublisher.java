package systems.zlink.samples.supportchat.server.support.infrastructure;

import systems.zlink.samples.supportchat.server.support.actors.SupportActorDirectory;
import systems.zlink.samples.supportchat.server.support.actors.SupportUserActor;
import systems.zlink.samples.supportchat.server.support.application.AgentAssignmentService;
import systems.zlink.samples.supportchat.server.support.domain.Conversation;
import systems.zlink.samples.supportchat.shared.contracts.Messages;

import java.util.Map;

public final class ConversationNotificationPublisher {
    // --8<-- [start:doc-sc-roster-push]
    public void assigned(
            AgentAssignmentService.AvailableAgent agent,
            Conversation.Snapshot state,
            SupportActorDirectory directory) {
        directory
                .require(agent.rosterActorId())
                .push(
                        new Messages.ConversationAssignedNotify(
                                state.conversationId(), ConversationContracts.state(state)));
    }

    // --8<-- [end:doc-sc-roster-push]

    public void publish(
            Conversation.Change change,
            Map<String, SupportUserActor> participants,
            AgentAssignmentService assignment) {
        for (Conversation.Event event : change.events()) {
            Messages.ConversationState state = ConversationContracts.state(event.state());
            switch (event.kind()) {
                case ParticipantJoined -> {
                    publishExcept(
                            participants,
                            event.actorId(),
                            new Messages.ParticipantJoinedNotify(
                                    state.conversationId(),
                                    event.actorId(),
                                    ConversationContracts.role(event.role()),
                                    state));
                }
                // --8<-- [start:doc-sc-message-push]
                case MessageAppended ->
                        publishExcept(
                                participants,
                                event.actorId(),
                                new Messages.ChatMessageNotify(
                                        state.conversationId(),
                                        ConversationContracts.message(event.message()),
                                        state));
                // --8<-- [end:doc-sc-message-push]
                case TypingChanged ->
                        publishExcept(
                                participants,
                                event.actorId(),
                                new Messages.TypingChangedNotify(
                                        state.conversationId(),
                                        event.actorId(),
                                        Boolean.TRUE.equals(event.typing()),
                                        state));
                case Idle ->
                        publishAll(
                                participants,
                                new Messages.ConversationIdleNotify(state.conversationId(), state));
                case Closed -> {
                    publishAll(
                            participants,
                            new Messages.ConversationClosedNotify(state.conversationId(), state));
                    assignment.releaseConversation(state.conversationId());
                }
            }
        }
    }

    private static void publishExcept(
            Map<String, SupportUserActor> participants, String actorId, Object notification) {
        participants.forEach(
                (participantId, actor) -> {
                    if (!participantId.equals(actorId)) {
                        actor.push(notification);
                    }
                });
    }

    private static void publishAll(
            Map<String, SupportUserActor> participants, Object notification) {
        participants.values().forEach(actor -> actor.push(notification));
    }
}
