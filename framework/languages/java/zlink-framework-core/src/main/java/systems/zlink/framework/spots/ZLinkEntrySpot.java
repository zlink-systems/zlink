package systems.zlink.framework.spots;

import systems.zlink.framework.actors.ZLinkActor;
import systems.zlink.framework.messaging.ZLinkMessage;

import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;

public interface ZLinkEntrySpot<TActor extends ZLinkActor>
        extends ZLinkSpotActorMembershipLifecycle<TActor> {
    ZLinkEntrySpotContext context();

    default void configure() {}

    default CompletionStage<Void> onInitialize() {
        return CompletableFuture.completedFuture(null);
    }

    default CompletionStage<Void> onClosing() {
        return CompletableFuture.completedFuture(null);
    }

    default CompletionStage<Void> onClosing(ZLinkSpotClosingContext context) {
        return onClosing();
    }

    default CompletionStage<ZLinkActorCreateResponse> onCreateActor(
            TActor actor, ZLinkMessage createRequest) {
        return CompletableFuture.completedFuture(ZLinkActorCreateResponse.accept());
    }
}
