package systems.zlink.samples.supportchat.server.support.spots.conversationspot;

import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import systems.zlink.framework.ZLinkMessageContext;
import systems.zlink.framework.spots.ZLinkSpotActorRequestHandler;
import systems.zlink.samples.supportchat.server.support.actors.SupportUserActor;
import systems.zlink.samples.supportchat.shared.contracts.Messages;

public final class CloseConversationHandler
    implements ZLinkSpotActorRequestHandler<ConversationSpot, SupportUserActor,
        Messages.CloseConversationReq, Messages.CloseConversationRes> {
    @Override
    public CompletionStage<Messages.CloseConversationRes> handle(
        ConversationSpot spot,
        SupportUserActor actor,
        ZLinkMessageContext context,
        Messages.CloseConversationReq request) {
        return CompletableFuture.completedFuture(spot.close(actor, request));
    }
}
