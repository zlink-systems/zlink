package systems.zlink.samples.supportchat.server.support.actors;

import systems.zlink.framework.actors.ZLinkActor;
import systems.zlink.framework.actors.ZLinkActorContext;
import systems.zlink.framework.actors.ZLinkActorJoinCompletion;
import systems.zlink.samples.supportchat.server.configuration.SampleTimings;
import systems.zlink.samples.supportchat.server.configuration.SampleNames;
import systems.zlink.samples.supportchat.shared.contracts.Messages;

public final class SupportUserActor implements ZLinkActor {
    private final String actorId;
    private final ZLinkActorContext context;
    private String displayName;
    private String role = "";
    private String participantId;
    private String conversationId = "";
    private String pendingConversationId;
    private final java.util.Set<systems.zlink.framework.actors.ZLinkActorJoinOperationId>
        completedJoinOperations = new java.util.HashSet<>();

    public SupportUserActor(String actorId, ZLinkActorContext context) {
        this.actorId = actorId;
        this.context = context;
        this.displayName = actorId;
        this.participantId = actorId;
    }

    public String actorId() {
        return actorId;
    }

    @Override
    public ZLinkActorContext context() {
        return context;
    }

    public String displayName() {
        return displayName;
    }

    public String role() {
        return role;
    }

    public String participantId() {
        return participantId;
    }

    public String conversationId() {
        return conversationId;
    }

    public String pendingConversationId() {
        return pendingConversationId == null ? "" : pendingConversationId;
    }

    public void setIdentity(String displayName, String role, String participantId) {
        this.displayName = displayName;
        this.role = role;
        this.participantId = participantId;
    }

    public void joinConversation(String conversationId) {
        this.conversationId = conversationId;
    }

    public void restorePendingConversationJoin(String conversationId) {
        pendingConversationId = conversationId == null || conversationId.isBlank()
            ? null
            : conversationId;
    }

    public java.util.Set<systems.zlink.framework.actors.ZLinkActorJoinOperationId>
        completedJoinOperations() {
        return java.util.Set.copyOf(completedJoinOperations);
    }

    public void restoreCompletedJoinOperations(
        java.util.Collection<systems.zlink.framework.actors.ZLinkActorJoinOperationId> operationIds) {
        completedJoinOperations.addAll(operationIds);
    }

    public Messages.JoinConversationRes scheduleConversationJoin(
        String conversationId,
        String subject,
        Messages.JoinConversationReq request) {
        if (pendingConversationId != null) {
            throw new IllegalStateException("A conversation join is already pending");
        }
        pendingConversationId = conversationId;
        context.joinSpot(conversationId, request)
            .timeout(SampleTimings.RequestTimeout)
            .defer();
        return new Messages.JoinConversationRes(true, new Messages.ConversationState(
            conversationId,
            subject,
            SampleNames.Statuses.WaitingForAgent,
            SampleNames.Roles.Customer.equals(request.role()) ? request.participantId() : "",
            null,
            0,
            null,
            null));
    }

    @Override
    public java.util.concurrent.CompletionStage<Void> onJoinCompleted(
        ZLinkActorJoinCompletion completion) {
        systems.zlink.framework.actors.ZLinkActorJoinOperationId operationId;
        if (completion instanceof ZLinkActorJoinCompletion.Accepted accepted) {
            operationId = accepted.operationId();
        } else if (completion instanceof ZLinkActorJoinCompletion.Rejected rejected) {
            operationId = rejected.operationId();
        } else {
            operationId = ((ZLinkActorJoinCompletion.Failed) completion).operationId();
        }
        if (!completedJoinOperations.add(operationId)) {
            return java.util.concurrent.CompletableFuture.completedFuture(null);
        }
        if (pendingConversationId == null) {
            return java.util.concurrent.CompletableFuture.completedFuture(null);
        }
        String pending = pendingConversationId;
        if (completion instanceof ZLinkActorJoinCompletion.Accepted accepted) {
            conversationId = pendingConversationId;
        }
        pendingConversationId = null;
        if (completion instanceof ZLinkActorJoinCompletion.Rejected) {
            return context.boundSession()
                .send(new Messages.JoinConversationFailedNotify(
                    pending,
                    "Rejected",
                    false))
                .metadata(SampleNames.ConversationIdMetadataKey, pending)
                .submit();
        }
        if (completion instanceof ZLinkActorJoinCompletion.Failed failed) {
            return context.boundSession()
                .send(new Messages.JoinConversationFailedNotify(
                    pending,
                    failed.kind().name(),
                    false))
                .metadata(SampleNames.ConversationIdMetadataKey, pending)
                .submit();
        }
        return java.util.concurrent.CompletableFuture.completedFuture(null);
    }

    public void push(Object message) {
        context.boundSession()
            .send(message)
            .submit();
    }
}
