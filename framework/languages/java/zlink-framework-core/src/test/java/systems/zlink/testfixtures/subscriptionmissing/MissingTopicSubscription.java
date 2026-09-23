package systems.zlink.testfixtures.subscriptionmissing;

import systems.zlink.framework.spots.ZLinkSpotSubscriptionHandler;

import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;

public final class MissingTopicSubscription
        implements ZLinkSpotSubscriptionHandler<Object, String> {
    @Override
    public CompletionStage<Void> handle(Object spot, String message) {
        return CompletableFuture.completedFuture(null);
    }
}
