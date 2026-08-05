package systems.zlink.samples.supportchat.server.support.spots.conversationspot;

import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import systems.zlink.framework.ZLinkMessageContext;
import systems.zlink.framework.spots.ZLinkSpotActorSendHandler;
import systems.zlink.samples.supportchat.server.support.actors.SupportUserActor;
import systems.zlink.samples.supportchat.shared.contracts.Messages;

public final class SetTypingHandler
    implements ZLinkSpotActorSendHandler<ConversationSpot, SupportUserActor, Messages.SetTypingReq> {
    @Override
    public CompletionStage<Void> handle(
        ConversationSpot spot,
        SupportUserActor actor,
        ZLinkMessageContext context,
        Messages.SetTypingReq request) {
        spot.setTyping(actor, request);
        return CompletableFuture.completedFuture(null);
    }
}
