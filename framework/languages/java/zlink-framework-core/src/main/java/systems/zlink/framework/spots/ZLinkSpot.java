package systems.zlink.framework.spots;

import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import systems.zlink.framework.actors.ZLinkActor;
import systems.zlink.framework.messaging.ZLinkMessage;

public interface ZLinkSpot<TActor extends ZLinkActor>
    extends ZLinkUserSpotActorLifecycle<TActor> {
    ZLinkSpotContext context();

    default void configure() {
    }

    default CompletionStage<ZLinkSpotCreateResponse> onCreate(ZLinkMessage request) {
        return CompletableFuture.completedFuture(ZLinkSpotCreateResponse.accept());
    }

    default CompletionStage<Void> onInitialize() {
        return CompletableFuture.completedFuture(null);
    }

    default CompletionStage<Void> onClosing() {
        return CompletableFuture.completedFuture(null);
    }

    default CompletionStage<Void> onClosing(
        ZLinkSpotClosingContext context) {
        return onClosing();
    }

    default CompletionStage<Void> onRelocationReadyCompleted(
        ZLinkSpotRelocationReadyCompletion completion) {
        return CompletableFuture.completedFuture(null);
    }

}
