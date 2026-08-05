package systems.zlink.samples.supportchat.server.support.infrastructure;

import java.util.concurrent.CompletionStage;
import systems.zlink.framework.spots.ZLinkSpotManager;
import systems.zlink.samples.supportchat.server.support.application.ConversationSpotFactory;
import systems.zlink.samples.supportchat.server.support.spots.conversationspot.ConversationCreateReq;
import systems.zlink.samples.supportchat.server.support.spots.conversationspot.ConversationSpot;
import systems.zlink.samples.supportchat.server.configuration.SampleNames;

public final class FrameworkConversationSpotFactory implements ConversationSpotFactory {
    private final ZLinkSpotManager spots;

    public FrameworkConversationSpotFactory(ZLinkSpotManager spots) {
        this.spots = spots;
    }

    @Override
    public CompletionStage<Void> start(String conversationId, StartRequest request) {
        return spots.getOrCreate(conversationId, SampleNames.ConversationSpotType)
            .request(new ConversationCreateReq(
                    request.customerActorId(), request.customerDisplayName(),
                    request.subject(), request.createdAtUnixMs()))
            .submit()
            .thenApply(ignored -> null);
    }
}
