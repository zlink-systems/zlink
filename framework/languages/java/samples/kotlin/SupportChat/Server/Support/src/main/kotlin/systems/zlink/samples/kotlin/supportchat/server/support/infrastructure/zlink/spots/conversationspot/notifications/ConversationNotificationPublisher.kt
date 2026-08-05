package systems.zlink.samples.kotlin.supportchat.server.support.infrastructure.zlink.spots.conversationspot.notifications

import systems.zlink.samples.kotlin.supportchat.server.configuration.SampleNames
import systems.zlink.samples.kotlin.supportchat.server.support.domain.ConversationEvent
import systems.zlink.samples.kotlin.supportchat.server.support.domain.ConversationEventKind
import systems.zlink.samples.kotlin.supportchat.server.support.domain.ConversationSnapshot
import systems.zlink.samples.kotlin.supportchat.server.support.infrastructure.ConversationContracts
import systems.zlink.samples.kotlin.supportchat.server.support.infrastructure.zlink.actors.SupportUserActor
import systems.zlink.samples.kotlin.supportchat.shared.contracts.ChatMessageNotify
import systems.zlink.samples.kotlin.supportchat.shared.contracts.ConversationAssignedNotify
import systems.zlink.samples.kotlin.supportchat.shared.contracts.ConversationClosedNotify
import systems.zlink.samples.kotlin.supportchat.shared.contracts.ConversationIdleNotify
import systems.zlink.samples.kotlin.supportchat.shared.contracts.ConversationState
import systems.zlink.samples.kotlin.supportchat.shared.contracts.ParticipantJoinedNotify
import systems.zlink.samples.kotlin.supportchat.shared.contracts.TypingChangedNotify

class ConversationNotificationPublisher {
    suspend fun publish(
        events: List<ConversationEvent>,
        actors: Map<String, SupportUserActor>,
    ) {
        for (event in events) {
            publish(event, actors)
        }
    }

    suspend fun publishAssignedToRoster(
        roster: SupportUserActor,
        snapshot: ConversationSnapshot,
    ) {
        val state = ConversationContracts.toState(snapshot)
        roster.context().boundSession()
            .send(ConversationAssignedNotify(state.conversationId, state))
            .submit()
    }

    private suspend fun publish(
        event: ConversationEvent,
        actors: Map<String, SupportUserActor>,
    ) {
        val state = ConversationContracts.toState(event.state)
        when (event.kind) {
            ConversationEventKind.ParticipantJoined -> publishParticipantJoined(event, state, actors)
            ConversationEventKind.MessageAppended -> publishMessage(event, state, actors)
            ConversationEventKind.TypingChanged -> publishTyping(event, state, actors)
            ConversationEventKind.Idle -> publishAll(actors.values) { actor ->
                actor.context().boundSession()
                    .send(ConversationIdleNotify(state.conversationId, state))
                    .submit()
            }
            ConversationEventKind.Closed -> publishAll(excludeActor(actors, event.actorId).values) { actor ->
                actor.context().boundSession()
                    .send(ConversationClosedNotify(state.conversationId, state))
                    .submit()
            }
        }
    }

    private suspend fun publishParticipantJoined(
        event: ConversationEvent,
        state: ConversationState,
        actors: Map<String, SupportUserActor>,
    ) {
        val actorId = event.actorId ?: error("Participant joined event requires actor id.")
        val role = event.role ?: error("Participant joined event requires role.")
        val customer = actors[state.customerActorId] ?: return
        if (customer.participantId == actorId) {
            return
        }
        customer.context().boundSession()
            .send(
                ParticipantJoinedNotify(
                    state.conversationId,
                    actorId,
                    ConversationContracts.toRole(role),
                    state,
                ),
            )
            .submit()
    }

    private suspend fun publishMessage(
        event: ConversationEvent,
        state: ConversationState,
        actors: Map<String, SupportUserActor>,
    ) {
        val message = event.message ?: error("Message event requires a chat message.")
        val chatMessage = ConversationContracts.toMessage(message)
        publishAll(excludeActor(actors, message.senderActorId).values) { actor ->
            actor.context().boundSession()
                .send(ChatMessageNotify(state.conversationId, chatMessage, state))
                .submit()
        }
    }

    private suspend fun publishTyping(
        event: ConversationEvent,
        state: ConversationState,
        actors: Map<String, SupportUserActor>,
    ) {
        val actorId = event.actorId ?: error("Typing event requires actor id.")
        val isTyping = event.isTyping ?: error("Typing event requires typing state.")
        publishAll(excludeActor(actors, actorId).values) { actor ->
            actor.context().boundSession()
                .send(TypingChangedNotify(state.conversationId, actorId, isTyping, state))
                .submit()
        }
    }

    private fun excludeActor(
        actors: Map<String, SupportUserActor>,
        actorId: String?,
    ): Map<String, SupportUserActor> =
        if (actorId == null) actors else actors.filterKeys { it != actorId }

    private suspend fun publishAll(
        actors: Collection<SupportUserActor>,
        publish: (SupportUserActor) -> Unit,
    ) {
        for (actor in actors) {
            publish(actor)
        }
    }
}
