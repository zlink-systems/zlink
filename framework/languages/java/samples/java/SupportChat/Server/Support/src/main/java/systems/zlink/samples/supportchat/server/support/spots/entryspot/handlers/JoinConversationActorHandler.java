package systems.zlink.samples.supportchat.server.support.spots.entryspot.handlers;

import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import systems.zlink.framework.ZLinkMessageContext;
import systems.zlink.framework.spots.ZLinkEntrySpotActorRequestHandler;
import systems.zlink.samples.supportchat.server.configuration.SampleNames;
import systems.zlink.samples.supportchat.server.support.actors.SupportUserActor;
import systems.zlink.samples.supportchat.server.support.spots.entryspot.SupportEntrySpot;
import systems.zlink.samples.supportchat.shared.contracts.Messages;

public final class JoinConversationActorHandler
    implements ZLinkEntrySpotActorRequestHandler<
        SupportEntrySpot,
        SupportUserActor,
        Messages.JoinConversationReq,
        Messages.JoinConversationRes> {
    @Override
    public CompletionStage<Messages.JoinConversationRes> handle(
        SupportEntrySpot spot,
        SupportUserActor actor,
        ZLinkMessageContext context,
        Messages.JoinConversationReq request) {
        if (!SampleNames.Roles.Agent.equals(actor.role())) {
            throw new IllegalStateException(
                "Only agent conversation actors can join through the Entry Spot");
        }
        String conversationId = context.metadata().get(
            SampleNames.ConversationIdMetadataKey);
        if (conversationId == null || conversationId.isBlank()) {
            throw new IllegalStateException(
                "Conversation Join is missing the conversation ID metadata");
        }
        Messages.JoinConversationRes scheduled = actor.scheduleConversationJoin(
            conversationId,
            "",
            new Messages.JoinConversationReq(
                actor.participantId(),
                actor.role(),
                actor.displayName()));
        return CompletableFuture.completedFuture(scheduled);
    }
}
