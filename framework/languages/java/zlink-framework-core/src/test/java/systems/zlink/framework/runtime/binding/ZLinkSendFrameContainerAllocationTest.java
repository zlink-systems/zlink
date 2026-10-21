package systems.zlink.framework.runtime.binding;

import static org.junit.jupiter.api.Assertions.*;

import com.sun.management.ThreadMXBean;

import jdk.jfr.Recording;
import jdk.jfr.consumer.RecordingFile;

import org.junit.jupiter.api.Test;

import systems.zlink.contracts.core.RoutingId;
import systems.zlink.contracts.core.Zlink;
import systems.zlink.contracts.messaging.Message;
import systems.zlink.framework.configuration.ZLinkApplicationJobQueueProfile;
import systems.zlink.framework.runtime.channels.ZLinkSendAdmissionAllocationTest;
import systems.zlink.framework.runtime.internal.dispatch.ZLinkApplicationJobQueue;
import systems.zlink.framework.runtime.messaging.ZLinkApplicationMetadata;

import java.lang.management.ManagementFactory;
import java.net.URLClassLoader;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.Collection;
import java.util.LinkedHashSet;
import java.util.List;
import java.util.OptionalLong;
import java.util.UUID;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicReference;

/** Measures the owned service-frame representation through a connected raw mesh send. */
public final class ZLinkSendFrameContainerAllocationTest {
    private static final int SAMPLES = 100;
    private static final byte[] PAYLOAD = new byte[1024];
    private static volatile List<Message> baselineFrames;

    @Test
    void sendOwnsOnlyItsFinalImmutableFrameList() throws Exception {
        var classpath = new LinkedHashSet<String>();
        for (ClassLoader loader = getClass().getClassLoader();
                loader != null;
                loader = loader.getParent()) {
            if (loader instanceof URLClassLoader urls) {
                for (var url : urls.getURLs()) classpath.add(Path.of(url.toURI()).toString());
            }
        }
        classpath.addAll(
                List.of(System.getProperty("java.class.path").split(java.io.File.pathSeparator)));
        Path output = Files.createTempFile("zlink-send-frames-", ".log");
        try {
            var process =
                    new ProcessBuilder(
                                    Path.of(System.getProperty("java.home"), "bin", "java")
                                            .toString(),
                                    "--enable-native-access=ALL-UNNAMED",
                                    "-Djava.io.tmpdir=" + System.getProperty("java.io.tmpdir"),
                                    "-XX:-UseTLAB",
                                    "-XX:-DoEscapeAnalysis",
                                    "-XX:ActiveProcessorCount=2",
                                    "-XX:FlightRecorderOptions=stackdepth=128",
                                    "-cp",
                                    String.join(java.io.File.pathSeparator, classpath),
                                    getClass().getName())
                            .redirectErrorStream(true)
                            .redirectOutput(output.toFile())
                            .start();
            int exit = process.waitFor();
            String log = Files.readString(output);
            System.out.print(log);
            assertEquals(0, exit, log);
        } finally {
            Files.deleteIfExists(output);
        }
    }

    public static void main(String[] args) throws Exception {
        if (args.length == 2 && args[0].equals("--benchmark")) {
            ZLinkSendAdmissionAllocationTest.benchmarkPaths(
                    Path.of(args[1]),
                    ZLinkSendFrameContainerAllocationTest.class,
                    List.of("2", "3"),
                    2_000);
            return;
        }
        long excessObjects = 0;
        long excessBytes = 0;
        for (boolean metadata : new boolean[] {false, true}) {
            try (Fixture fixture = new Fixture()) {
                fixture.sendBatch(200, metadata);
                long[] baseline = recordContainers(() -> baselineBatch(metadata), true);
                long[] actual = recordContainers(() -> fixture.sendBatch(SAMPLES, metadata), false);
                System.out.printf(
                        "arity=%d baselineObjects=%d actualObjects=%d baselineBytes=%d actualBytes=%d sends=%d%n",
                        metadata ? 3 : 2, baseline[0], actual[0], baseline[1], actual[1], SAMPLES);
                assertTrue(baseline[0] > 0, "the ordinary immutable list is the positive control");
                excessObjects += Math.abs(actual[0] - baseline[0]);
                excessBytes += Math.abs(actual[1] - baseline[1]);
            }
        }
        assertEquals(
                0,
                excessObjects,
                "no mutable or copied frame container beyond the immutable baseline");
        assertEquals(
                0, excessBytes, "no frame-container bytes beyond the same-JVM immutable baseline");
    }

    private static void baselineBatch(boolean metadata) {
        try (Message header = Message.from("header");
                Message attributes = Message.from("metadata");
                Message body = Message.from("body")) {
            for (int index = 0; index < SAMPLES; index++) {
                baselineList(header, attributes, body, metadata);
            }
        }
    }

    private static void baselineList(
            Message header, Message metadata, Message body, boolean included) {
        baselineFrames =
                List.copyOf(included ? List.of(header, metadata, body) : List.of(header, body));
    }

    @FunctionalInterface
    interface Action {
        void run() throws Exception;
    }

    private static long[] recordContainers(Action action, boolean baseline) throws Exception {
        Path file = Files.createTempFile("zlink-frame-containers-", ".jfr");
        try {
            try (Recording recording = new Recording()) {
                recording.enable("jdk.ObjectAllocationOutsideTLAB").withStackTrace();
                recording.enable("jdk.DataLoss");
                recording.start();
                action.run();
                recording.stop();
                recording.dump(file);
            }
            long objects = 0;
            long bytes = 0;
            try (var events = new RecordingFile(file)) {
                while (events.hasMoreEvents()) {
                    var event = events.readEvent();
                    assertNotEquals("jdk.DataLoss", event.getEventType().getName());
                    if (!event.getEventType().getName().equals("jdk.ObjectAllocationOutsideTLAB"))
                        continue;
                    String className = event.getClass("objectClass").getName();
                    if ((!className.startsWith("java.util.") && !className.startsWith("["))
                            || className.contains("$$Lambda")) continue;
                    Class<?> type = Class.forName(className);
                    boolean container =
                            Collection.class.isAssignableFrom(type)
                                    || (type.isArray() && !type.getComponentType().isPrimitive());
                    if (!container || event.getStackTrace() == null) continue;
                    String owner = null;
                    String method = null;
                    for (var frame : event.getStackTrace().getFrames()) {
                        String candidate = frame.getMethod().getType().getName();
                        if (candidate.startsWith("systems.zlink.framework.")) {
                            owner = candidate;
                            method = frame.getMethod().getName();
                            break;
                        }
                    }
                    boolean included =
                            baseline
                                    ? ZLinkSendFrameContainerAllocationTest.class
                                                    .getName()
                                                    .equals(owner)
                                            && "baselineList".equals(method)
                                    : (ZLinkJavaRawMeshNode.class.getName().equals(owner)
                                                    && "encodeApplicationFrames".equals(method))
                                            || (ZLinkJavaRawServicePort.class
                                                            .getName()
                                                            .equals(owner)
                                                    && "claimMessages".equals(method));
                    if (included) {
                        objects++;
                        bytes += event.getLong("allocationSize");
                    }
                }
            }
            return new long[] {objects, bytes};
        } finally {
            Files.deleteIfExists(file);
        }
    }

    /**
     * Same benchmark signature as the admission fixture, called only through its public surface.
     */
    public static long[] measureSendPath(String arity, int count) throws Exception {
        ThreadMXBean allocations = (ThreadMXBean) ManagementFactory.getThreadMXBean();
        try (Fixture fixture = new Fixture()) {
            long before = allocations.getCurrentThreadAllocatedBytes();
            long cpuStart = allocations.getCurrentThreadCpuTime();
            long start = System.nanoTime();
            fixture.sendBatch(count, arity.equals("3"));
            return new long[] {
                System.nanoTime() - start,
                allocations.getCurrentThreadAllocatedBytes() - before,
                allocations.getCurrentThreadCpuTime() - cpuStart
            };
        }
    }

    static final class Fixture implements AutoCloseable {
        final systems.zlink.contracts.core.Context context = Zlink.createContext();
        final ZLinkApplicationJobQueue queue =
                new ZLinkApplicationJobQueue(
                        ZLinkApplicationJobQueueProfile.BALANCED,
                        OptionalLong.empty(),
                        new ZLinkApplicationJobQueue.ProcessorCandidates(1, null, null, null));
        final ZLinkJavaRawMeshNode source = new ZLinkJavaRawMeshNode(context, "send-frames");
        final ZLinkJavaRawMeshNode target = new ZLinkJavaRawMeshNode(context, "send-frames");
        final RoutingId targetId = RoutingId.from("send-frames-target");
        final AtomicReference<Throwable> receiveFailure = new AtomicReference<>();
        final byte[] metadata = ZLinkApplicationMetadata.empty().with("trace", "value").encode();
        volatile CountDownLatch deliveries;

        Fixture() throws Exception {
            try {
                String endpoint = "inproc://send-frames-" + UUID.randomUUID();
                source.setRoutingId(RoutingId.from("send-frames-source"));
                source.setBind(endpoint + "-source");
                target.setRoutingId(targetId);
                target.setBind(endpoint);
                target.setApplicationJobQueue(queue);
                target.startDispatch(
                        record -> {
                            try (record) {
                                assertArrayEquals(PAYLOAD, record.parts().getFirst().toByteArray());
                            } catch (Throwable failure) {
                                receiveFailure.compareAndSet(null, failure);
                            } finally {
                                deliveries.countDown();
                            }
                        });
                source.start();
                target.start();
                var ready = new CompletableFuture<Void>();
                try (var observation =
                        source.onStateChanged(
                                () -> {
                                    if (source.readyPeerCount() > 0) ready.complete(null);
                                })) {
                    source.connectPeer(endpoint, targetId);
                    ready.get(10, TimeUnit.SECONDS);
                }
            } catch (Exception | Error failure) {
                try {
                    close();
                } catch (RuntimeException | Error cleanupFailure) {
                    failure.addSuppressed(cleanupFailure);
                }
                throw failure;
            }
        }

        void sendBatch(int count, boolean included) throws Exception {
            deliveries = new CountDownLatch(count);
            for (int index = 0; index < count; index++) {
                try (Message payload = Message.from(PAYLOAD)) {
                    source.spotNode()
                            .sendToNode(targetId, included ? metadata : null, List.of(payload))
                            .toCompletableFuture()
                            .join();
                }
            }
            assertTrue(deliveries.await(10, TimeUnit.SECONDS), "all sends must be delivered");
            assertNull(receiveFailure.get(), "received payload remains unchanged");
        }

        @Override
        public void close() {
            try (context;
                    queue;
                    target;
                    source) {
                // Close every owner even if an earlier resource reports failure.
            }
        }
    }
}
