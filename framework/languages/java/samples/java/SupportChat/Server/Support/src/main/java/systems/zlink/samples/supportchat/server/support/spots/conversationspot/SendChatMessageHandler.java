package systems.zlink.samples.supportchat.server.support.spots.conversationspot;

import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import systems.zlink.framework.ZLinkMessageContext;
import systems.zlink.framework.spots.ZLinkSpotActorRequestHandler;
import systems.zlink.samples.supportchat.server.support.actors.SupportUserActor;
import systems.zlink.samples.supportchat.shared.contracts.Messages;

public final class SendChatMessageHandler
    implements ZLinkSpotActorRequestHandler<ConversationSpot, SupportUserActor,
        Messages.SendChatMessageReq, Messages.SendChatMessageRes> {
    @Override
    public CompletionStage<Messages.SendChatMessageRes> handle(
        ConversationSpot spot,
        SupportUserActor actor,
        ZLinkMessageContext context,
        Messages.SendChatMessageReq request) {
        return CompletableFuture.completedFuture(spot.sendMessage(actor, request));
    }
}
