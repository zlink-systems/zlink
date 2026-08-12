package systems.zlink.framework.runtime.host;

import static org.junit.jupiter.api.Assertions.assertTrue;

import java.lang.ref.WeakReference;
import java.time.Duration;
import java.util.UUID;
import java.util.concurrent.Flow;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicInteger;
import org.junit.jupiter.api.Test;
import systems.zlink.framework.monitoring.ZLinkMeshNodeSnapshot;
import systems.zlink.framework.monitoring.ZLinkObservedStatus;
import systems.zlink.framework.runtime.binding.ZLinkJavaBackendAdapterFactory;
import systems.zlink.framework.runtime.configuration.DefaultZLinkFrameworkOptions;

/**
 * {@code observe(mesh, capacity).subscribe(subscriber)} is the natural call
 * shape: nothing outside the runtime keeps the returned publisher, and the
 * subscriber drops its Subscription. The observation has to keep working after
 * the collector runs.
 */
final class ZLinkRouteMeshObserveRetentionTest {
    private static final String MESH = "gc-observed-mesh";

    @Test
    void meshObservationKeepsDeliveringAfterGarbageCollection()
        throws Exception {
        DefaultZLinkFrameworkOptions options =
            new DefaultZLinkFrameworkOptions();
        options.addRouteMesh(MESH)
            .listen("inproc://" + MESH + "-" + UUID.randomUUID());
        ZLinkFrameworkRuntime runtime = ZLinkFrameworkRuntimeTestAccess.start(
            options,
            new ZLinkJavaBackendAdapterFactory());
        try {
            awaitReady(runtime);
            AtomicInteger delivered = new AtomicInteger();
            AtomicInteger stopping = new AtomicInteger();
            // Neither the publisher nor the Subscription is stored anywhere:
            // the subscription itself is what has to keep the publisher alive.
            runtime.routeMeshRuntime().observe(MESH, 32).subscribe(
                new Flow.Subscriber<ZLinkObservedStatus<ZLinkMeshNodeSnapshot>>() {
                    @Override
                    public void onSubscribe(Flow.Subscription subscription) {
                        subscription.request(Long.MAX_VALUE);
                    }

                    @Override
                    public void onNext(
                        ZLinkObservedStatus<ZLinkMeshNodeSnapshot> status) {
                        delivered.incrementAndGet();
                        switch (status.status().state()) {
                            case STOPPING, STOPPED -> stopping.incrementAndGet();
                            default -> {
                            }
                        }
                    }

                    @Override
                    public void onError(Throwable failure) {
                    }

                    @Override
                    public void onComplete() {
                    }
                });
            awaitAtLeast(delivered, 1, "the first mesh snapshot");
            int beforeCollection = delivered.get();

            forceCollection();

            runtime.shutdown(Duration.ofSeconds(2))
                .toCompletableFuture()
                .get(5, TimeUnit.SECONDS);
            awaitAtLeast(
                delivered,
                beforeCollection + 1,
                "a mesh snapshot observed after the collector ran");
            assertTrue(stopping.get() > 0,
                "the terminal mesh states have to reach a live subscription");
        } finally {
            runtime.close();
        }
    }

    private static void awaitReady(ZLinkFrameworkRuntime runtime)
        throws InterruptedException {
        long deadline = System.nanoTime() + Duration.ofSeconds(5).toNanos();
        while (!runtime.isReady() && System.nanoTime() < deadline) {
            Thread.sleep(1);
        }
    }

    /**
     * Runs the collector until a sentinel is proven cleared, so the assertion
     * below never depends on a hint that the runtime was free to ignore.
     */
    private static void forceCollection() throws InterruptedException {
        for (int attempt = 0; attempt < 8; attempt++) {
            WeakReference<Object> sentinel = new WeakReference<>(new Object());
            long deadline = System.nanoTime() + Duration.ofSeconds(5).toNanos();
            while (!sentinel.refersTo(null) && System.nanoTime() < deadline) {
                System.gc();
                Thread.sleep(10);
            }
        }
    }

    private static void awaitAtLeast(
        AtomicInteger counter,
        int expected,
        String what) throws InterruptedException {
        long deadline = System.nanoTime() + Duration.ofSeconds(10).toNanos();
        while (counter.get() < expected && System.nanoTime() < deadline) {
            Thread.sleep(5);
        }
        assertTrue(counter.get() >= expected,
            "timed out waiting for " + what
                + " (observed " + counter.get() + ")");
    }
}
