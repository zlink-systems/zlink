package systems.zlink.framework.runtime.binding;

import static org.junit.jupiter.api.Assertions.*;

import jdk.jfr.Recording;
import jdk.jfr.consumer.RecordingFile;

import org.junit.jupiter.api.Test;

import systems.zlink.contracts.messaging.Message;
import systems.zlink.framework.runtime.internal.service.ZLinkServiceSendHeaderAllocationTest;
import systems.zlink.framework.runtime.internal.service.ZLinkServiceTopologyRegistry;

import java.nio.file.Files;
import java.util.Optional;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.TimeUnit;
import java.util.function.Supplier;

/** Selection already on its owner returns the existing peer without adapter objects. */
public final class ZLinkSendSelectionAllocationTest {
    private static final int SENDS = 100;
    private static final String CHANNEL = "send-selection";
    private static final byte[] PAYLOAD = new byte[1024];
    private static volatile Object observed;

    @Test
    void ownerChannelSendUsesThePreparedPeerDirectly() throws Exception {
        ZLinkServiceSendHeaderAllocationTest.runInAllocationJvm(getClass());
    }

    public static void main(String[] args) throws Exception {
        if (args.length == 2 && args[0].equals("--benchmark")) {
            systems.zlink.framework.runtime.internal.service.ZLinkServiceSendHeaderAllocationTest
                    .benchmarkPaths(
                            java.nio.file.Path.of(args[1]),
                            ZLinkSendSelectionAllocationTest.class,
                            java.util.List.of("channel"),
                            10_000);
            return;
        }
        try (var fixture = new ZLinkSendFrameContainerAllocationTest.Fixture(CHANNEL)) {
            send(fixture, 200);
            long[] control =
                    record(
                            () -> {
                                byte[] value = PAYLOAD;
                                for (int i = 0; i < SENDS; i++) {
                                    observed = Optional.of(value);
                                    observed = (Supplier<byte[]>) () -> value;
                                }
                            },
                            true);
            long[] actual = record(() -> send(fixture, SENDS), false);
            System.out.printf(
                    "sends=%d controlOptionals=%d selectionOptionals=%d controlSuppliers=%d selectionSuppliers=%d%n",
                    SENDS, control[0], actual[0], control[1], actual[1]);
            assertEquals(SENDS, control[0]);
            assertEquals(SENDS, control[1]);
            assertEquals(0, actual[0], "owner selection uses the existing peer directly");
            assertEquals(0, actual[1], "owner selection does not need a captured turn adapter");
        }
    }

    private static void send(ZLinkSendFrameContainerAllocationTest.Fixture fixture, int count)
            throws Exception {
        fixture.deliveries = new CountDownLatch(count);
        for (int i = 0; i < count; i++) {
            try (var payload = Message.from(PAYLOAD)) {
                fixture.source
                        .spotNode()
                        .sendToChannel(CHANNEL, null, java.util.List.of(payload))
                        .toCompletableFuture()
                        .join();
            }
        }
        assertTrue(fixture.deliveries.await(10, TimeUnit.SECONDS));
        assertNull(fixture.receiveFailure.get());
    }

    private static ZLinkSendFrameContainerAllocationTest.Fixture benchmarkFixture;

    public static void setup(int unusedPortBase) throws Exception {
        benchmarkFixture = new ZLinkSendFrameContainerAllocationTest.Fixture(CHANNEL);
    }

    public static void cleanup() throws Exception {
        if (benchmarkFixture != null) {
            benchmarkFixture.close();
            benchmarkFixture = null;
        }
    }

    public static long[] measureSendPath(String path, int count) throws Exception {
        var metrics =
                (com.sun.management.ThreadMXBean)
                        java.lang.management.ManagementFactory.getThreadMXBean();
        boolean temporary = benchmarkFixture == null;
        var fixture =
                temporary
                        ? new ZLinkSendFrameContainerAllocationTest.Fixture(CHANNEL)
                        : benchmarkFixture;
        try {
            long before = metrics.getCurrentThreadAllocatedBytes();
            long cpu = metrics.getCurrentThreadCpuTime();
            long start = System.nanoTime();
            send(fixture, count);
            return new long[] {
                System.nanoTime() - start,
                metrics.getCurrentThreadAllocatedBytes() - before,
                metrics.getCurrentThreadCpuTime() - cpu
            };
        } finally {
            if (temporary) fixture.close();
        }
    }

    private interface Action {
        void run() throws Exception;
    }

    private static long[] record(Action action, boolean control) throws Exception {
        var file = Files.createTempFile("zlink-send-selection-", ".jfr");
        try {
            try (var recording = new Recording()) {
                recording.enable("jdk.ObjectAllocationOutsideTLAB").withStackTrace();
                recording.enable("jdk.DataLoss");
                recording.start();
                action.run();
                recording.stop();
                recording.dump(file);
            }
            long[] count = new long[2];
            try (var events = new RecordingFile(file)) {
                while (events.hasMoreEvents()) {
                    var event = events.readEvent();
                    assertNotEquals("jdk.DataLoss", event.getEventType().getName());
                    if (!event.getEventType().getName().equals("jdk.ObjectAllocationOutsideTLAB")
                            || event.getStackTrace() == null) continue;
                    String owner = null;
                    for (var frame : event.getStackTrace().getFrames()) {
                        String candidate = frame.getMethod().getType().getName();
                        if (candidate.startsWith("systems.zlink.framework.")) {
                            owner = candidate;
                            break;
                        }
                    }
                    boolean included =
                            control
                                    ? event.getStackTrace().getFrames().stream()
                                            .anyMatch(
                                                    frame ->
                                                            frame.getMethod()
                                                                            .getType()
                                                                            .getName()
                                                                            .equals(
                                                                                    ZLinkSendSelectionAllocationTest
                                                                                            .class
                                                                                            .getName())
                                                                    && frame.getMethod()
                                                                            .getName()
                                                                            .startsWith(
                                                                                    "lambda$main$"))
                                    : owner != null
                                            && owner.startsWith(
                                                    ZLinkServiceTopologyRegistry.class.getName())
                                            && event.getStackTrace().getFrames().stream()
                                                    .anyMatch(
                                                            frame ->
                                                                    frame.getMethod()
                                                                                    .getType()
                                                                                    .getName()
                                                                                    .equals(
                                                                                            ZLinkJavaRawMeshNode
                                                                                                    .class
                                                                                                    .getName())
                                                                            && frame.getMethod()
                                                                                    .getName()
                                                                                    .equals(
                                                                                            "sendChannel"));
                    if (!included) continue;
                    String type = event.getClass("objectClass").getName();
                    if (type.equals(Optional.class.getName())) count[0]++;
                    if (type.contains("$$Lambda")) count[1]++;
                }
            }
            return count;
        } finally {
            Files.deleteIfExists(file);
        }
    }
}
