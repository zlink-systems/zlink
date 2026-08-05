package systems.zlink.framework.runtime.binding;

import static org.junit.jupiter.api.Assertions.assertDoesNotThrow;
import static org.junit.jupiter.api.Assertions.assertArrayEquals;
import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertThrows;

import java.time.Duration;
import java.util.List;
import java.util.Optional;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;
import java.util.concurrent.TimeUnit;
import org.junit.jupiter.api.Test;
import systems.zlink.contracts.core.RoutingId;

final class ZLinkJavaRawServicePortContractTest {
    @Test
    void closesOwnedRawResourcesOnceAndRejectsNewResourcesAfterClose() {
        ZLinkJavaRawServicePort port = new ZLinkJavaRawServicePort();
        port.openRouter(RoutingId.from("m5-resource-owner"));

        port.close();

        assertDoesNotThrow(port::close);
        assertThrows(IllegalStateException.class,
            () -> port.openRouter(RoutingId.from("after-close")));
    }

    @Test
    void sendsAndReceivesCopiedMultipartWithSourceRoutingId() throws Exception {
        RoutingId leftRid = RoutingId.from("m5-left");
        RoutingId rightRid = RoutingId.from("m5-right");
        String endpoint = "inproc://m5-raw-service-port-" + System.nanoTime();
        byte[] first = new byte[] {1, 2, 3};
        byte[] second = new byte[] {4, 5, 6};

        try (ZLinkJavaRawServicePort port = new ZLinkJavaRawServicePort()) {
            var left = port.openRouter(leftRid);
            var right = port.openRouter(rightRid);
            left.bind(endpoint);
            right.connect(endpoint);

            long deadline = System.nanoTime() + Duration.ofSeconds(2).toNanos();
            while (!port.send(right, leftRid, List.of(first, second))) {
                if (System.nanoTime() >= deadline) {
                    throw new AssertionError("raw multipart send did not become ready");
                }
                Thread.sleep(1);
            }
            first[0] = 9;
            second[0] = 9;

            Optional<ZLinkJavaRawServicePort.Inbound> received = Optional.empty();
            while (received.isEmpty()) {
                received = port.receive(left);
                if (System.nanoTime() >= deadline) {
                    throw new AssertionError("raw multipart receive timed out");
                }
                if (received.isEmpty()) {
                    Thread.sleep(1);
                }
            }

            assertEquals(rightRid, received.orElseThrow().source());
            List<byte[]> retained = received.orElseThrow().frames();
            assertArrayEquals(new byte[] {1, 2, 3}, retained.get(0));
            assertArrayEquals(new byte[] {4, 5, 6}, retained.get(1));
            retained.get(0)[0] = 8;
            assertArrayEquals(
                new byte[] {1, 2, 3},
                received.orElseThrow().frames().get(0));
        }
    }

    @Test
    void serializesConcurrentMultipartSubmitsOnOneRouter() throws Exception {
        RoutingId leftRid = RoutingId.from("m5-concurrent-left");
        RoutingId rightRid = RoutingId.from("m5-concurrent-right");
        String endpoint = "inproc://m5-raw-service-port-concurrent-"
            + System.nanoTime();

        try (ZLinkJavaRawServicePort port = new ZLinkJavaRawServicePort();
             ExecutorService executor = Executors.newFixedThreadPool(2)) {
            var left = port.openRouter(leftRid);
            var right = port.openRouter(rightRid);
            left.bind(endpoint);
            right.connect(endpoint);

            long connectDeadline =
                System.nanoTime() + Duration.ofSeconds(2).toNanos();
            while (!port.send(right, leftRid, List.of(new byte[] {0}))) {
                if (System.nanoTime() >= connectDeadline) {
                    throw new AssertionError("raw concurrent route did not become ready");
                }
                Thread.sleep(1);
            }
            awaitInbound(port, left, 1, connectDeadline);

            CountDownLatch start = new CountDownLatch(1);
            var first = executor.submit(() -> sendMany(
                port, right, leftRid, (byte) 1, start));
            var second = executor.submit(() -> sendMany(
                port, right, leftRid, (byte) 2, start));
            start.countDown();

            first.get(5, TimeUnit.SECONDS);
            second.get(5, TimeUnit.SECONDS);
            awaitInbound(port, left, 32,
                System.nanoTime() + Duration.ofSeconds(2).toNanos());
        }
    }

    private static void sendMany(
        ZLinkJavaRawServicePort port,
        systems.zlink.contracts.sockets.RouterSocket router,
        RoutingId target,
        byte marker,
        CountDownLatch start) {
        try {
            start.await();
            for (int index = 0; index < 16; index++) {
                long deadline =
                    System.nanoTime() + Duration.ofSeconds(2).toNanos();
                while (!port.send(
                    router,
                    target,
                    List.of(new byte[] {marker, (byte) index}))) {
                    if (System.nanoTime() >= deadline) {
                        throw new AssertionError(
                            "concurrent raw multipart send timed out");
                    }
                    Thread.yield();
                }
            }
        } catch (InterruptedException interrupted) {
            Thread.currentThread().interrupt();
            throw new AssertionError("concurrent raw multipart send interrupted", interrupted);
        }
    }

    private static void awaitInbound(
        ZLinkJavaRawServicePort port,
        systems.zlink.contracts.sockets.RouterSocket router,
        int expected,
        long deadline) throws Exception {
        int receivedCount = 0;
        while (receivedCount < expected) {
            if (port.receive(router).isPresent()) {
                receivedCount++;
                continue;
            }
            if (System.nanoTime() >= deadline) {
                throw new AssertionError(
                    "raw concurrent multipart receive timed out");
            }
            Thread.sleep(1);
        }
    }
}
