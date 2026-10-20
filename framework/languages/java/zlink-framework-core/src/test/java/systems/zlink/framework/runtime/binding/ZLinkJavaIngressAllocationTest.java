package systems.zlink.framework.runtime.binding;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertNotNull;
import static org.junit.jupiter.api.Assertions.assertTrue;

import jdk.jfr.Recording;
import jdk.jfr.consumer.RecordingFile;

import org.junit.jupiter.api.Test;

import systems.zlink.contracts.core.RoutingId;
import systems.zlink.contracts.core.Zlink;
import systems.zlink.contracts.messaging.Message;
import systems.zlink.framework.configuration.ZLinkApplicationJobQueueProfile;
import systems.zlink.framework.runtime.internal.backend.ZLinkMeshApplicationReceiver;
import systems.zlink.framework.runtime.internal.backend.ZLinkMeshDispatchRecord;
import systems.zlink.framework.runtime.internal.dispatch.ZLinkApplicationJobQueue;

import java.net.URLClassLoader;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.Path;
import java.time.Duration;
import java.util.LinkedHashSet;
import java.util.List;
import java.util.OptionalLong;
import java.util.UUID;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import java.util.concurrent.TimeUnit;

/** Counts actual ingress allocations, independently of object layout and TLAB sampling. */
public final class ZLinkJavaIngressAllocationTest {
    private static final int REQUESTS = 1000;
    private static final int WARMUP = 200;
    private static final Duration REQUEST_TIMEOUT = Duration.ofSeconds(5);

    @Test
    void ingressAllocatesOnlyItsRecordAndLaneInvocation() throws Exception {
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
        Path output = Files.createTempFile("zlink-ingress-allocation-", ".log");
        try {
            Process process =
                    new ProcessBuilder(
                                    Path.of(System.getProperty("java.home"), "bin", "java")
                                            .toString(),
                                    "--enable-native-access=ALL-UNNAMED",
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
            String log = Files.readString(output, StandardCharsets.UTF_8);
            System.out.print(log);
            assertEquals(0, exit, log);
        } finally {
            Files.deleteIfExists(output);
        }
    }

    public static void main(String[] args) throws Exception {
        Path recordingPath = Files.createTempFile("zlink-ingress-", ".jfr");
        try {
            exercise(recordingPath);
            long records = 0;
            long attempts = 0;
            long portObjects = 0;
            long resumeCallbacks = 0;
            long byteIterators = 0;
            long managementIterators = 0;
            long managementCallbacks = 0;
            try (var events = new RecordingFile(recordingPath)) {
                while (events.hasMoreEvents()) {
                    var event = events.readEvent();
                    if (!event.getEventType().getName().equals("jdk.ObjectAllocationOutsideTLAB"))
                        continue;
                    String allocated = event.getClass("objectClass").getName();
                    if (!(allocated.startsWith("systems.zlink.framework.runtime.binding.")
                            || allocated.equals("systems.zlink.contracts.messaging.Received")
                            || allocated.endsWith("$Itr")
                            || allocated.endsWith("$ListItr"))) continue;
                    var stack = event.getStackTrace();
                    if (stack == null) continue;
                    boolean receive = false;
                    boolean bytes = false;

                    String firstFrameworkMethod = null;
                    for (var frame : stack.getFrames()) {
                        var method = frame.getMethod();
                        String owner = method.getType().getName();
                        if (firstFrameworkMethod == null
                                && owner.startsWith("systems.zlink.framework."))
                            firstFrameworkMethod = method.getName();
                        if (owner.equals(ZLinkJavaRawServicePort.class.getName())
                                && method.getName().equals("receiveNow")) receive = true;
                        if (owner.equals(ZLinkJavaRawMeshNode.class.getName())) {
                            bytes |= method.getName().equals("retainedBytes");
                        }
                    }
                    if (receive) {
                        if (allocated.equals("systems.zlink.contracts.messaging.Received"))
                            attempts++;
                        if (allocated.equals(ZLinkJavaRawServicePort.Inbound.class.getName()))
                            records++;
                        if (allocated.startsWith(ZLinkJavaRawServicePort.class.getName()))
                            portObjects++;
                    }
                    if ("drainIngressBatch".equals(firstFrameworkMethod)
                            && allocated.startsWith(ZLinkJavaRawMeshNode.class.getName())
                            && allocated.contains("$$Lambda")) resumeCallbacks++;
                    if (bytes && allocated.endsWith("$Itr")) byteIterators++;
                    if ("tickLiveness".equals(firstFrameworkMethod)
                            && allocated.endsWith("$ListItr")) managementIterators++;
                    if ("tickLiveness".equals(firstFrameworkMethod)
                            && allocated.contains("$$Lambda")) managementCallbacks++;
                }
            }
            System.out.printf(
                    "records=%d attempts=%d portObjects=%d resumeCallbacks=%d byteIterators=%d managementIterators=%d managementCallbacks=%d%n",
                    records,
                    attempts,
                    portObjects,
                    resumeCallbacks,
                    byteIterators,
                    managementIterators,
                    managementCallbacks);
            assertTrue(
                    records >= REQUESTS,
                    "include all requests and any ordinary management records");
            assertTrue(attempts >= records, "include no-data claims in the lane-call baseline");
            // A native record needs one Framework owner; each claim needs one lane invocation.
            // Object sizes, alignment, binding allocations and JDK container layouts are excluded.
            assertEquals(
                    attempts + records, portObjects, "no separate header-list owner per record");
            assertEquals(0, resumeCallbacks, "immediate permits need no resumption closure");
            assertEquals(0, byteIterators, "byte accounting indexes the existing message list");
            assertEquals(0, managementIterators, "management lists need no cursor object");
            assertEquals(0, managementCallbacks, "timeout iteration needs no capturing callback");
        } finally {
            Files.deleteIfExists(recordingPath);
        }
    }

    private static void exercise(Path recordingPath) throws Exception {
        try (var queue =
                        new ZLinkApplicationJobQueue(
                                ZLinkApplicationJobQueueProfile.BALANCED,
                                OptionalLong.of(64),
                                new ZLinkApplicationJobQueue.ProcessorCandidates(
                                        1, null, null, null),
                                100,
                                0);
                var context = Zlink.createContext();
                var source = new ZLinkJavaRawMeshNode(context, "ingress-cost");
                var target = new ZLinkJavaRawMeshNode(context, "ingress-cost")) {
            var sourceId = RoutingId.from("ingress-cost-source");
            var targetId = RoutingId.from("ingress-cost-target");
            String endpoint = "inproc://ingress-cost-target-" + UUID.randomUUID();
            target.setApplicationJobQueue(queue);
            target.setApplicationReceiver(
                    new ZLinkMeshApplicationReceiver() {
                        @Override
                        public void accept(ZLinkMeshDispatchRecord record) {
                            try (record;
                                    var reply = Message.from(new byte[1024])) {
                                record.reply(List.of(reply));
                            }
                        }

                        @Override
                        public CompletionStage<Integer> submitLocalNodeSend(
                                RoutingId sourceNodeRid, byte[] metadata, List<Message> parts) {
                            throw new AssertionError("remote ingress fixture");
                        }
                    });
            source.setRoutingId(sourceId);
            target.setRoutingId(targetId);
            source.setBind("inproc://ingress-cost-source-" + UUID.randomUUID());
            target.setBind(endpoint);
            source.start();
            target.start();
            var ready = new CompletableFuture<Void>();
            try (var observer =
                    source.onPeerStateChanged(
                            targetId,
                            () -> {
                                if (source.observePeerRoute(targetId).ready()) ready.complete(null);
                            },
                            () -> {})) {
                source.connectPeer(endpoint, targetId);
                ready.get(5, TimeUnit.SECONDS);
            }
            requests(source, targetId, WARMUP);
            try (var recording = new Recording()) {
                recording.enable("jdk.ObjectAllocationOutsideTLAB").withStackTrace();
                recording.start();
                requests(source, targetId, REQUESTS);
                recording.stop();
                recording.dump(recordingPath);
            }
        }
    }

    private static void requests(ZLinkJavaRawMeshNode source, RoutingId target, int count)
            throws Exception {
        for (int i = 0; i < count; i++) {
            try (var packet = Message.from("allocation.packet");
                    var body = Message.from(new byte[1024])) {
                try (var reply =
                        source.requestNode(
                                        target,
                                        null,
                                        List.of(packet, body),
                                        REQUEST_TIMEOUT,
                                        null,
                                        null,
                                        null)
                                .toCompletableFuture()
                                .get(5, TimeUnit.SECONDS)) {
                    assertNotNull(reply);
                    assertEquals(1024, reply.parts().getFirst().size());
                }
            }
        }
    }
}
