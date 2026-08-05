package systems.zlink.framework.runtime.spots;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertFalse;
import static org.junit.jupiter.api.Assertions.assertTrue;

import java.lang.reflect.Proxy;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.List;
import java.util.Optional;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicInteger;
import org.junit.jupiter.api.Test;
import systems.zlink.contracts.messaging.Message;
import systems.zlink.contracts.errors.ZlinkSubmitException;
import systems.zlink.contracts.sockets.SubmitResult;

import systems.zlink.framework.runtime.internal.backend.ZLinkInternalSpotNode;
import systems.zlink.framework.runtime.messaging.ZLinkApplicationMetadata;
import systems.zlink.framework.runtime.messaging.ZLinkStringMessageSerializer;

final class ZLinkSpotPublisherRuntimeTest {
    @Test
    void committedPublishCompletesNormallyWithoutPublishMonitoring()
        throws Exception {
        AtomicInteger coreCalls = new AtomicInteger();
        AtomicInteger publishMetrics = new AtomicInteger();
        ZLinkSpotPublisherRuntime runtime = runtime(coreCalls::incrementAndGet);
        try (
            runtime;
            AutoCloseable ignored =
                systems.zlink.framework.runtime.internal.metrics.ZLinkRuntimeMetrics
                    .install(new systems.zlink.framework.runtime.internal.metrics
                        .ZLinkRuntimeMetrics.Sink() {
                        @Override
                        public void add(
                            String name,
                            long delta,
                            java.util.Map<String, String> tags) {
                            if (name.contains("multicast")
                                || name.equals("zlink.fanout.published")) {
                                publishMetrics.incrementAndGet();
                            }
                        }
                    })
        ) {
            ZLinkOneWayPublishAdmission result = submit(runtime, "partial").join();

            assertEquals(0, result.status());
            assertEquals(0, publishMetrics.get());
            assertTrue(awaitCount(coreCalls, 1));
            assertEquals(1, coreCalls.get());
        }
    }

    @Test
    void targetSubmissionFailureAfterStartRemainsSuccessful() {
        ZLinkSpotPublisherRuntime runtime = runtime(() -> {
            throw new ZlinkSubmitException(SubmitResult.NOT_CONNECTED);
        });
        try (runtime) {
            ZLinkOneWayPublishAdmission result = submit(runtime, "unreachable").join();

            assertEquals(0, result.status());
        }
    }

    @Test
    void noSubscriberAfterStartCompletesNormally() throws Exception {
        AtomicInteger coreCalls = new AtomicInteger();
        ZLinkSpotPublisherRuntime runtime = runtime(() -> {
            coreCalls.incrementAndGet();
            throw new ZlinkSubmitException(SubmitResult.NOT_FOUND);
        });
        try (runtime) {
            ZLinkOneWayPublishAdmission result = submit(runtime, "missing").join();

            assertEquals(0, result.status());
            assertTrue(awaitCount(coreCalls, 1));
            assertEquals(1, coreCalls.get());
        }
    }

    @Test
    void cancellationAfterCommitDoesNotReplaceTheCorePublishResult() throws Exception {
        CountDownLatch committed = new CountDownLatch(1);
        CountDownLatch release = new CountDownLatch(1);
        AtomicInteger coreCalls = new AtomicInteger();
        ZLinkSpotPublisherRuntime runtime = runtime(() -> {
            coreCalls.incrementAndGet();
            committed.countDown();
            await(release);
        });
        try (runtime) {
            CompletableFuture<ZLinkOneWayPublishAdmission> result = submit(runtime, "committed");
            assertTrue(committed.await(1, TimeUnit.SECONDS));

            assertFalse(result.cancel(false));
            assertTrue(result.isDone());
            assertEquals(0, result.join().status());
            release.countDown();

            assertEquals(1, coreCalls.get());
        } finally {
            release.countDown();
        }
    }

    @Test
    void shutdownAfterCommitDoesNotReplaceTheCorePublishResult() throws Exception {
        CountDownLatch committed = new CountDownLatch(1);
        CountDownLatch release = new CountDownLatch(1);
        AtomicInteger coreCalls = new AtomicInteger();
        ZLinkSpotPublisherRuntime runtime = runtime(() -> {
            coreCalls.incrementAndGet();
            committed.countDown();
            awaitUninterruptibly(release);
        });
        try {
            CompletableFuture<ZLinkOneWayPublishAdmission> result = submit(runtime, "committed");
            assertTrue(committed.await(1, TimeUnit.SECONDS));
            assertTrue(result.isDone());
            assertEquals(0, result.join().status());

            runtime.close();
            release.countDown();

            assertEquals(1, coreCalls.get());
        } finally {
            release.countDown();
            runtime.close();
        }
    }

    @Test
    void boundedExecutorHandoffWaitsForWorkerCapacity() throws Exception {
        int workerCount = Math.max(2, Runtime.getRuntime().availableProcessors());
        CountDownLatch started = new CountDownLatch(workerCount);
        CountDownLatch release = new CountDownLatch(1);
        AtomicInteger coreCalls = new AtomicInteger();
        ZLinkSpotPublisherRuntime runtime = runtime(() -> {
            coreCalls.incrementAndGet();
            started.countDown();
            await(release);
        });
        List<CompletableFuture<ZLinkOneWayPublishAdmission>> committed = new ArrayList<>();
        try (runtime) {
            for (int index = 0; index < workerCount; index++) {
                committed.add(submit(runtime, "held-" + index));
            }
            assertTrue(started.await(2, TimeUnit.SECONDS));

            CompletableFuture<ZLinkOneWayPublishAdmission> queued =
                submit(runtime, "queued");
            assertFalse(queued.isDone());
            assertEquals(workerCount, coreCalls.get());
            release.countDown();
            committed.forEach(CompletableFuture::join);
            assertEquals(0, queued.join().status());
            assertEquals(workerCount + 1, coreCalls.get());
        } finally {
            release.countDown();
        }
    }

    @Test
    void saturatedHandoffIsBoundedTimesOutAndRecovers() throws Exception {
        CountDownLatch started = new CountDownLatch(1);
        CountDownLatch release = new CountDownLatch(1);
        ZLinkSpotPublisherRuntime runtime = runtime(
            () -> {
                started.countDown();
                await(release);
            },
            1);
        List<CompletableFuture<ZLinkOneWayPublishAdmission>> pending =
            new ArrayList<>();
        try (runtime) {
            pending.add(submit(runtime, "worker"));
            assertTrue(started.await(1, TimeUnit.SECONDS));
            pending.add(submit(runtime, "handoff-worker"));

            ZLinkOneWayPublishAdmission overflow =
                submit(runtime, "bounded-overflow").join();
            assertEquals(
                2,
                overflow.status());
            assertEquals(
                2,
                pending.get(1).get(2, TimeUnit.SECONDS).status());

            release.countDown();
            for (CompletableFuture<ZLinkOneWayPublishAdmission> result : pending) {
                result.get(2, TimeUnit.SECONDS);
            }
            assertEquals(
                0,
                submit(runtime, "recovered").get(2, TimeUnit.SECONDS).status());
        } finally {
            release.countDown();
        }
    }

    @Test
    void closedRuntimeRejectsNewPublishAsShutdownWithoutCoreCall() {
        AtomicInteger coreCalls = new AtomicInteger();
        ZLinkSpotPublisherRuntime runtime = runtime(() -> {
            coreCalls.incrementAndGet();
        });
        runtime.close();

        ZLinkOneWayPublishAdmission result = submit(runtime, "after-close").join();

        assertEquals(5, result.status());
        assertEquals(0, coreCalls.get());
    }

    private static CompletableFuture<ZLinkOneWayPublishAdmission> submit(
        ZLinkSpotPublisherRuntime runtime,
        String payload) {
        return runtime.submitAsync(
                "mesh",
                "channel",
                "topic",
                Message.from(payload.getBytes(StandardCharsets.UTF_8)),
                Optional.empty(),
                ZLinkApplicationMetadata.empty())
            .toCompletableFuture();
    }

    private static ZLinkSpotPublisherRuntime runtime(Runnable publish) {
        return runtime(
            publish,
            Math.max(2, Runtime.getRuntime().availableProcessors()));
    }

    private static ZLinkSpotPublisherRuntime runtime(
        Runnable publish,
        int parallelism) {
        ZLinkStringMessageSerializer serializer = new ZLinkStringMessageSerializer();
        ZLinkSpotPublisherRuntime runtime = new ZLinkSpotPublisherRuntime(
            serializer,
            new ZLinkSpotRouteMessages(serializer),
            parallelism,
            ignored -> java.time.Duration.ofMillis(100));
        AtomicInteger proxyClose = new AtomicInteger();
        ZLinkInternalSpotNode node = (ZLinkInternalSpotNode) Proxy.newProxyInstance(
            ZLinkInternalSpotNode.class.getClassLoader(),
            new Class<?>[] {ZLinkInternalSpotNode.class},
            (ignored, method, arguments) -> switch (method.getName()) {
                case "publish" -> {
                    publish.run();
                    yield null;
                }
                case "name" -> "publisher-node";
                case "close" -> {
                    proxyClose.incrementAndGet();
                    yield null;
                }
                default -> defaultValue(method.getReturnType());
            });
        runtime.register("mesh", node);
        return runtime;
    }

    private static Object defaultValue(Class<?> type) {
        if (!type.isPrimitive()) {
            return null;
        }
        if (type == boolean.class) {
            return false;
        }
        if (type == char.class) {
            return '\0';
        }
        return 0;
    }

    private static void await(CountDownLatch latch) {
        try {
            latch.await();
        } catch (InterruptedException error) {
            Thread.currentThread().interrupt();
            throw new AssertionError(error);
        }
    }

    private static void awaitUninterruptibly(CountDownLatch latch) {
        boolean interrupted = false;
        while (true) {
            try {
                latch.await();
                break;
            } catch (InterruptedException ignored) {
                interrupted = true;
            }
        }
        if (interrupted) {
            Thread.currentThread().interrupt();
        }
    }

    private static boolean awaitCount(AtomicInteger value, int expected)
        throws InterruptedException {
        long deadline = System.nanoTime() + TimeUnit.SECONDS.toNanos(2);
        while (value.get() != expected && System.nanoTime() < deadline) {
            Thread.sleep(1);
        }
        return value.get() == expected;
    }
}
