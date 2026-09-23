package systems.zlink.testfixtures.subscriptionvalid;

import systems.zlink.framework.handlers.ZLinkSpotSubscription;
import systems.zlink.framework.spots.ZLinkSpotSubscriptionHandler;

import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;

@ZLinkSpotSubscription(topic = "room.events")
public final class TopicSubscription implements ZLinkSpotSubscriptionHandler<Object, String> {
    @Override
    public CompletionStage<Void> handle(Object spot, String message) {
        return CompletableFuture.completedFuture(null);
    }
}
