package systems.zlink.samples.supportchat.server.support.spots.conversationspot;

import java.time.Duration;
import java.util.LinkedHashMap;
import java.util.Map;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import org.slf4j.Logger;
import org.slf4j.LoggerFactory;
import systems.zlink.framework.messaging.ZLinkMessage;
import systems.zlink.framework.spots.ZLinkSpot;
import systems.zlink.framework.spots.ZLinkSpotActorJoinResult;
import systems.zlink.framework.spots.ZLinkSpotContext;
import systems.zlink.framework.spots.ZLinkSpotCreateResponse;
import systems.zlink.framework.spots.ZLinkTimer;
import systems.zlink.samples.supportchat.server.configuration.SampleNames;
import systems.zlink.samples.supportchat.server.configuration.SampleTimings;
import systems.zlink.samples.supportchat.server.support.actors.SupportActorDirectory;
import systems.zlink.samples.supportchat.server.support.actors.SupportUserActor;
import systems.zlink.samples.supportchat.server.support.application.AgentAssignmentService;
import systems.zlink.samples.supportchat.server.support.domain.Conversation;
import systems.zlink.samples.supportchat.server.support.infrastructure.ConversationContracts;
import systems.zlink.samples.supportchat.server.support.infrastructure.ConversationNotificationPublisher;
import systems.zlink.samples.supportchat.shared.contracts.Messages;

public final class ConversationSpot implements ZLinkSpot<SupportUserActor> {
    private static final Logger logger = LoggerFactory.getLogger(ConversationSpot.class);
    private final ZLinkSpotContext context;
    private final AgentAssignmentService assignment;
    private final SupportActorDirectory directory;
    private final ConversationNotificationPublisher notifications;
    private final Map<String, SupportUserActor> actors = new LinkedHashMap<>();
    private final java.util.Set<String> pendingJoins = new java.util.HashSet<>();
    private Conversation conversation;
    private ZLinkTimer idleTimer;

    public ConversationSpot(
        ZLinkSpotContext context,
        AgentAssignmentService assignment,
        SupportActorDirectory directory,
        ConversationNotificationPublisher notifications) {
        this.context = context;
        this.assignment = assignment;
        this.directory = directory;
        this.notifications = notifications;
    }

    @Override
    public ZLinkSpotContext context() {
        return context;
    }

    @Override
    public CompletionStage<ZLinkSpotCreateResponse> onCreate(ZLinkMessage request) {
        ConversationCreateReq create = request.decode(ConversationCreateReq.class);
        conversation = new Conversation(
            context.spotId(),
            create.subject(),
            create.customerActorId(),
            create.customerDisplayName(),
            create.createdAtUnixMs(),
            new Conversation.Policy(
                SampleTimings.IdleTimeout,
                SampleTimings.CloseGraceTimeout,
                500));
        logger.info(
            "support conversation: created. conversation={}, customer={}",
            context.spotId(), create.customerActorId());
        return CompletableFuture.completedFuture(ZLinkSpotCreateResponse.accept());
    }

    @Override
    public CompletionStage<Void> onInitialize() {
        return context.addTimer(
                "conversation-idle",
                Duration.ofMillis(200),
                ConversationIdleTimerHandler.class,
                null)
            .thenAccept(timer -> idleTimer = timer);
    }

    @Override
    public CompletionStage<Void> onClosing() {
        return idleTimer == null
            ? CompletableFuture.completedFuture(null)
            : idleTimer.cancel();
    }

    @Override
    public CompletionStage<ZLinkSpotActorJoinResult> onActorJoin(
        String actorId,
        ZLinkMessage request) {
        request.decode(Messages.JoinConversationReq.class);
        pendingJoins.add(actorId);
        return CompletableFuture.completedFuture(ZLinkSpotActorJoinResult.accept(
            new Messages.JoinConversationRes(
                false,
                ConversationContracts.state(requireConversation().snapshot()))));
    }

    @Override
    public CompletionStage<Void> onJoinedActor(SupportUserActor actor) {
        if (!pendingJoins.remove(actor.actorId())) {
            return CompletableFuture.failedFuture(
                new IllegalStateException("joined actor does not have pending admission"));
        }
        directory.remember(actor);
        if (SampleNames.Roles.Agent.equals(actor.role())) {
            publish(requireConversation().joinAgent(
                actor.participantId(), actor.displayName(), System.currentTimeMillis()));
        } else {
            actor.joinConversation(requireConversation().snapshot().conversationId());
            actors.put(actor.participantId(), actor);
            assignAgent();
        }
        logger.info(
            "support conversation: actor joined. conversation={}, participant={}, role={}",
            context.spotId(), actor.participantId(), actor.role());
        return CompletableFuture.completedFuture(null);
    }

    @Override
    public CompletionStage<Void> onLeaveActor(SupportUserActor actor) {
        pendingJoins.remove(actor.actorId());
        actors.remove(actor.participantId());
        return CompletableFuture.completedFuture(null);
    }

    public Messages.JoinConversationRes refreshMembership(SupportUserActor actor) {
        SupportUserActor participant = SampleNames.Roles.Agent.equals(actor.role())
            ? directory.require(actor.participantId()) : actor;
        actors.put(actor.participantId(), participant);
        return new Messages.JoinConversationRes(
            false,
            ConversationContracts.state(requireConversation().snapshot()));
    }

    public Messages.SendChatMessageRes sendMessage(
        SupportUserActor actor,
        Messages.SendChatMessageReq request) {
        Conversation.Change change = requireConversation().sendMessage(
            actor.participantId(), request.text(), System.currentTimeMillis());
        publish(change);
        Conversation.Message message = change.events().stream()
            .map(Conversation.Event::message)
            .filter(java.util.Objects::nonNull)
            .findFirst()
            .orElseThrow();
        return new Messages.SendChatMessageRes(
            ConversationContracts.message(message), ConversationContracts.state(change.state()));
    }

    public void setTyping(SupportUserActor actor, Messages.SetTypingReq request) {
        publish(requireConversation().setTyping(actor.participantId(), request.isTyping()));
    }

    public Messages.CloseConversationRes close(
        SupportUserActor actor,
        Messages.CloseConversationReq request) {
        Conversation.Change change = requireConversation().close(actor.participantId(), request.reason());
        publish(change);
        return new Messages.CloseConversationRes(ConversationContracts.state(change.state()));
    }

    public void checkIdle() {
        publish(requireConversation().markIdle(System.currentTimeMillis()));
    }

    private void assignAgent() {
        AgentAssignmentService.AvailableAgent assigned = assignment.assignForConversation(
            requireConversation().snapshot().conversationId());
        if (assigned == null) {
            return;
        }
        notifications.assigned(assigned, requireConversation().snapshot(), directory);
        logger.info(
            "support conversation: assigned. conversation={}, roster={}",
            context.spotId(), assigned.rosterActorId());
    }

    private void publish(Conversation.Change change) {
        notifications.publish(change, actors, directory, assignment);
        for (Conversation.Event event : change.events()) {
            Messages.ConversationState state = ConversationContracts.state(event.state());
            logger.info(
                "support conversation: state changed. conversation={}, status={}, event={}",
                state.conversationId(), state.status(), event.kind());
        }
    }

    private Conversation requireConversation() {
        if (conversation == null) {
            throw new IllegalStateException("Conversation has not been created");
        }
        return conversation;
    }
}
