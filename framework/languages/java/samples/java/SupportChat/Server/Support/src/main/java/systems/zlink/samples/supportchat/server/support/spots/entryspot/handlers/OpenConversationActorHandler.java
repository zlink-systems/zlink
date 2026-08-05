package systems.zlink.samples.supportchat.server.support.spots.entryspot.handlers;

import systems.zlink.framework.spots.ZLinkEntrySpotActorRequestHandler;
import systems.zlink.framework.ZLinkMessageContext;
import systems.zlink.samples.supportchat.server.configuration.SampleNames;
import systems.zlink.samples.supportchat.server.configuration.SampleTimings;
import systems.zlink.samples.supportchat.server.support.actors.SupportUserActor;
import systems.zlink.samples.supportchat.server.support.spots.entryspot.SupportEntrySpot;
import systems.zlink.samples.supportchat.shared.contracts.Messages;

public final class OpenConversationActorHandler
    implements ZLinkEntrySpotActorRequestHandler<
        SupportEntrySpot,
        SupportUserActor,
        Messages.OpenConversationReq,
        Messages.OpenConversationRes> {
    @Override
    public java.util.concurrent.CompletionStage<Messages.OpenConversationRes> handle(
        SupportEntrySpot spot,
        SupportUserActor actor,
        ZLinkMessageContext context,
        Messages.OpenConversationReq request) {
        requireRole(actor, SampleNames.Roles.Customer);
        return spot.context().outbound()
            .requestToChannel(
                SampleNames.ApiChannel,
                new Messages.OpenConversationApiReq(actor.actorId(), actor.displayName(), request.subject()))
            .timeout(SampleTimings.RequestTimeout)
            .submit(Messages.OpenConversationApiRes.class)
            .thenApply(opened -> {
                Messages.JoinConversationRes scheduled = actor
                .scheduleConversationJoin(
                    opened.conversationId(),
                    request.subject(),
                    new Messages.JoinConversationReq(
                        actor.participantId(), actor.role(), actor.displayName()));
                return new Messages.OpenConversationRes(opened.conversationId(), scheduled.state());
            });
    }

    private static void requireRole(SupportUserActor actor, String expectedRole) {
        if (!expectedRole.equals(actor.role())) {
            throw new IllegalStateException("Expected role " + expectedRole + " but got " + actor.role());
        }
    }
}
