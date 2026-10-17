package systems.zlink.framework.runtime.host;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertNotSame;
import static org.junit.jupiter.api.Assertions.assertSame;

import org.junit.jupiter.api.Test;

import systems.zlink.framework.monitoring.ZLinkFrameworkRuntimeStatus;
import systems.zlink.framework.monitoring.ZLinkObservedStatus;
import systems.zlink.framework.runtime.configuration.DefaultZLinkFrameworkOptions;

import java.time.Duration;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.Flow;
import java.util.concurrent.TimeUnit;

final class ZLinkFrameworkRuntimeStatusPublicationTest {
    private static final Duration WAIT = Duration.ofSeconds(5);

    @Test
    void unchangedQueriesReuseThePublishedStatusAndCapacityResetPublishesOnce() throws Exception {
        try (var runtime =
                ZLinkFrameworkRuntimeTestAccess.start(new DefaultZLinkFrameworkOptions())) {
            ZLinkFrameworkRuntimeTestAccess.startupCompletion(runtime)
                    .toCompletableFuture()
                    .get(WAIT.toSeconds(), TimeUnit.SECONDS);
            var initial = runtime.status();
            assertSame(initial, runtime.status());
            assertEquals(initial.sequence(), runtime.status().sequence());
            assertEquals(initial.observedAt(), runtime.status().observedAt());

            runtime.resetCapacityMetrics();
            var reset = runtime.status();
            assertNotSame(initial, reset);
            assertEquals(
                    initial.capacity().measurementEpoch() + 1, reset.capacity().measurementEpoch());
            assertEquals(initial.sequence() + 1, reset.sequence());
            assertSame(reset, runtime.status());

            var firstObserved = new CompletableFuture<ZLinkFrameworkRuntimeStatus>();
            runtime.observe()
                    .subscribe(
                            new Flow.Subscriber<
                                    ZLinkObservedStatus<ZLinkFrameworkRuntimeStatus>>() {
                                private Flow.Subscription subscription;

                                @Override
                                public void onSubscribe(Flow.Subscription subscription) {
                                    this.subscription = subscription;
                                    subscription.request(1);
                                }

                                @Override
                                public void onNext(
                                        ZLinkObservedStatus<ZLinkFrameworkRuntimeStatus> observed) {
                                    subscription.cancel();
                                    firstObserved.complete(observed.status());
                                }

                                @Override
                                public void onError(Throwable failure) {
                                    firstObserved.completeExceptionally(failure);
                                }

                                @Override
                                public void onComplete() {}
                            });
            assertSame(reset, firstObserved.get(WAIT.toSeconds(), TimeUnit.SECONDS));
            assertSame(reset, runtime.status());
        }
    }
}
