package systems.zlink.framework.testsupport;

import systems.zlink.framework.monitoring.ZLinkObservedStatus;

import java.util.concurrent.CompletableFuture;
import java.util.concurrent.Flow;
import java.util.function.Predicate;

/** Waits for a matching status and cancels the subscription when the wait ends. */
public final class ZLinkStatusAwait {
    private ZLinkStatusAwait() {}

    public static <T> CompletableFuture<T> matching(
            Flow.Publisher<ZLinkObservedStatus<T>> publisher, Predicate<T> condition) {
        var result = new CompletableFuture<T>();
        publisher.subscribe(
                new Flow.Subscriber<ZLinkObservedStatus<T>>() {
                    @Override
                    public void onSubscribe(Flow.Subscription subscription) {
                        result.whenComplete((status, failure) -> subscription.cancel());
                        subscription.request(Long.MAX_VALUE);
                    }

                    @Override
                    public void onNext(ZLinkObservedStatus<T> observed) {
                        if (condition.test(observed.status())) {
                            result.complete(observed.status());
                        }
                    }

                    @Override
                    public void onError(Throwable failure) {
                        result.completeExceptionally(failure);
                    }

                    @Override
                    public void onComplete() {
                        result.completeExceptionally(
                                new IllegalStateException(
                                        "Observation completed before the status matched"));
                    }
                });
        return result;
    }
}
