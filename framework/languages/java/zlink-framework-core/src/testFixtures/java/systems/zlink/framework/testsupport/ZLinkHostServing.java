package systems.zlink.framework.testsupport;

import systems.zlink.framework.monitoring.ZLinkFrameworkRuntimeStatus;
import systems.zlink.framework.monitoring.ZLinkObservedStatus;
import systems.zlink.framework.runtime.host.ZLinkFrameworkRuntime;

import java.util.concurrent.CompletableFuture;
import java.util.concurrent.Flow;

/**
 * Waits for a host's own Serving status. A fixed-RID host finishes startup in Preparing and serves
 * only after 3·margin (MeshNode §3.3), so callers wait for that status before sending work.
 */
public final class ZLinkHostServing {
    private ZLinkHostServing() {}

    public static void await(ZLinkFrameworkRuntime runtime) throws Exception {
        var ready = new CompletableFuture<Void>();
        runtime.observe()
                .subscribe(
                        new Flow.Subscriber<ZLinkObservedStatus<ZLinkFrameworkRuntimeStatus>>() {
                            private Flow.Subscription subscription;

                            public void onSubscribe(Flow.Subscription value) {
                                subscription = value;
                                value.request(Long.MAX_VALUE);
                            }

                            public void onNext(
                                    ZLinkObservedStatus<ZLinkFrameworkRuntimeStatus> value) {
                                if (value.status().isReady()) {
                                    ready.complete(null);
                                    subscription.cancel();
                                }
                            }

                            public void onError(Throwable failure) {
                                ready.completeExceptionally(failure);
                            }

                            public void onComplete() {
                                ready.completeExceptionally(
                                        new IllegalStateException(
                                                "Host observation completed before READY"));
                            }
                        });
        ready.get();
    }
}
