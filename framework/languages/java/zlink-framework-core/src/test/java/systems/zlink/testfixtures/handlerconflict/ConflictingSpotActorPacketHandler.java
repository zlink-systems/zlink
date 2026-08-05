package systems.zlink.testfixtures.handlerconflict;

import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import systems.zlink.framework.actors.ZLinkActor;
import systems.zlink.framework.actors.ZLinkActorContext;
import systems.zlink.framework.handlers.ZLinkSpotActorRequest;
import systems.zlink.framework.handlers.ZLinkSpotActorSend;
import systems.zlink.framework.spots.ZLinkSpot;
import systems.zlink.framework.ZLinkMessageContext;
import systems.zlink.framework.spots.ZLinkSpotContext;

public final class ConflictingSpotActorPacketHandler {
    @ZLinkSpotActorSend(packetName = "SpotActorSend")
    @ZLinkSpotActorRequest(packetName = "SpotActorRequest")
    public CompletionStage<SpotActorReply> handle(
        TestSpot spot,
        TestActor actor,
        ZLinkMessageContext context,
        SpotActorRequest request) {
        return CompletableFuture.completedFuture(new SpotActorReply());
    }

    public static final class TestSpot implements ZLinkSpot<ZLinkActor> {
        @Override
        public ZLinkSpotContext context() {
            throw new UnsupportedOperationException();
        }

        @Override public CompletionStage<Void> onJoinedActor(ZLinkActor actor) {
            return CompletableFuture.completedFuture(null);
        }
        @Override public CompletionStage<Void> onLeaveActor(ZLinkActor actor) {
            return CompletableFuture.completedFuture(null);
        }
    }

    public static final class TestActor implements ZLinkActor {
        @Override
        public ZLinkActorContext context() {
            throw new UnsupportedOperationException();
        }
    }

    public record SpotActorRequest() {
    }

    public record SpotActorReply() {
    }
}
