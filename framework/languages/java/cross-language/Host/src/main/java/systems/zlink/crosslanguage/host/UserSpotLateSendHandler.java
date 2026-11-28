package systems.zlink.crosslanguage.host;

import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import systems.zlink.crosslanguage.host.UserSpotJoinContracts.UserSpotLateSend;
import systems.zlink.framework.ZLinkMessageContext;
import systems.zlink.framework.spots.ZLinkSpotActorSendHandler;

public final class UserSpotLateSendHandler implements ZLinkSpotActorSendHandler<
    RelocationUserSpot, RelocationActor, UserSpotLateSend> {
    private final EventSink sink;

    public UserSpotLateSendHandler(EventSink sink) {
        this.sink = sink;
    }

    @Override
    public CompletionStage<Void> handle(RelocationUserSpot spot, RelocationActor actor,
        ZLinkMessageContext context, UserSpotLateSend message) {
        sink.append("late-actor-send-executed|actor=" + actor.actorId() + "|marker=" + message.marker());
        return CompletableFuture.completedFuture(null);
    }
}
