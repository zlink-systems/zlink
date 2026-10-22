package systems.zlink.framework.runtime.binding;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertTrue;

import jdk.jfr.Recording;
import jdk.jfr.consumer.RecordedStackTrace;
import jdk.jfr.consumer.RecordingFile;

import org.junit.jupiter.api.Test;

import systems.zlink.contracts.core.RoutingId;
import systems.zlink.framework.channels.ZLinkRouteMessageContext;
import systems.zlink.framework.channels.ZLinkRouteRequestHandler;
import systems.zlink.framework.runtime.configuration.DefaultZLinkFrameworkOptions;
import systems.zlink.framework.runtime.host.ZLinkFrameworkRuntime;
import systems.zlink.framework.runtime.host.ZLinkFrameworkRuntimeTestAccess;

import java.net.ServerSocket;
import java.net.URLClassLoader;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.Path;
import java.time.Duration;
import java.util.LinkedHashSet;
import java.util.List;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import java.util.concurrent.TimeUnit;

/**
 * Counts the binding messages one public RouteMesh request creates, through real Framework
 * runtimes. Spec 05 §1/§4: received parts are read in place from the retained frame; a typed
 * request and reply materialize no further binding message for them.
 */
public final class ZLinkRouteRequestPartCostTest {
    private static final int WARMUP = 200;
    private static final int REQUESTS = 500;
    private static final String MESH = "part-cost";
    private static final Duration GUARD = Duration.ofSeconds(10);
    private static final RoutingId SOURCE = RoutingId.from("part-cost-source");
    private static final RoutingId TARGET = RoutingId.from("part-cost-target");
    private static final String MESSAGE = "systems.zlink.contracts.messaging.Message";

    @Test
    void receivedRequestAndReplyPartsAreReadWithoutMaterializedMessages() throws Exception {
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
        Path output = Files.createTempFile("zlink-route-request-parts-", ".log");
        try {
            Process process =
                    new ProcessBuilder(
                                    Path.of(System.getProperty("java.home"), "bin", "java")
                                            .toString(),
                                    "--enable-native-access=ALL-UNNAMED",
                                    "-Djava.io.tmpdir=" + System.getProperty("java.io.tmpdir"),
                                    "-XX:-UseTLAB",
                                    "-XX:-DoEscapeAnalysis",
                                    "-XX:ActiveProcessorCount=2",
                                    "-XX:FlightRecorderOptions=stackdepth=256",
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
        Path recordingPath = Files.createTempFile("zlink-route-request-parts-", ".jfr");
        try {
            exercise(recordingPath);
            long messages = 0;
            long materialized = 0;
            long truncated = 0;
            try (var events = new RecordingFile(recordingPath)) {
                while (events.hasMoreEvents()) {
                    var event = events.readEvent();
                    assertTrue(
                            !event.getEventType().getName().equals("jdk.DataLoss"),
                            "allocation recording must not lose events");
                    if (!event.getEventType().getName().equals("jdk.ObjectAllocationOutsideTLAB")
                            || !event.getClass("objectClass").getName().equals(MESSAGE)) continue;
                    messages++;
                    RecordedStackTrace stack = event.getStackTrace();
                    if (stack == null || stack.isTruncated()) {
                        truncated++;
                        continue;
                    }
                    for (var frame : stack.getFrames()) {
                        var method = frame.getMethod();
                        if (method.getType().getName().endsWith(".ZLinkFrameworkMultipartView")
                                && (method.getName().equals("get")
                                        || method.getName().equals("detachAll"))) {
                            materialized++;
                            break;
                        }
                    }
                }
            }
            System.out.printf(
                    "requests=%d binding_messages=%d per_request=%.3f materialized_parts=%d"
                            + " truncated=%d%n",
                    REQUESTS, messages, messages / (double) REQUESTS, materialized, truncated);
            assertEquals(0, truncated, "every message allocation needs its owner stack");
            assertTrue(messages >= REQUESTS, "the recording covers the measured requests");
            assertEquals(
                    0,
                    materialized,
                    "a typed request and reply read their received parts from the retained frame");
        } finally {
            Files.deleteIfExists(recordingPath);
        }
    }

    private static void exercise(Path recordingPath) throws Exception {
        int port;
        try (var socket = new ServerSocket(0)) {
            port = socket.getLocalPort();
        }
        String endpoint = "tcp://127.0.0.1:" + port;
        var targetOptions = new DefaultZLinkFrameworkOptions();
        targetOptions
                .addRouteMesh(MESH)
                .listen(endpoint)
                .setRoutingId(TARGET)
                .addRouteRequestHandler(Echo.class, Probe.class, Probe.class);
        try (var target = ZLinkFrameworkRuntimeTestAccess.start(targetOptions)) {
            var sourceOptions = new DefaultZLinkFrameworkOptions();
            sourceOptions
                    .addRouteMesh(MESH)
                    .listen("tcp://127.0.0.1:0")
                    .setRoutingId(SOURCE)
                    .peerConnections()
                    .connect(TARGET, endpoint);
            try (var source = ZLinkFrameworkRuntimeTestAccess.start(sourceOptions)) {
                long guard = System.nanoTime() + GUARD.toNanos();
                while (source.routeMeshRuntime().snapshot(MESH).readyPeerCount() != 1) {
                    if (System.nanoTime() >= guard)
                        throw new AssertionError("route mesh peer did not become ready");
                    Thread.sleep(1);
                }
                requests(source, WARMUP);
                try (var recording = new Recording()) {
                    recording.enable("jdk.ObjectAllocationOutsideTLAB").withStackTrace();
                    recording.enable("jdk.DataLoss");
                    recording.start();
                    requests(source, REQUESTS);
                    recording.stop();
                    recording.dump(recordingPath);
                }
            }
        }
    }

    private static void requests(ZLinkFrameworkRuntime source, int count) throws Exception {
        for (int index = 0; index < count; index++) {
            String value = "probe-" + index;
            var reply =
                    source.route()
                            .requestToNode(MESH, TARGET, new Probe(value))
                            .submit(Probe.class)
                            .toCompletableFuture()
                            .get(GUARD.toMillis(), TimeUnit.MILLISECONDS);
            assertEquals(value, reply.value());
        }
    }

    public record Probe(String value) {}

    public static final class Echo implements ZLinkRouteRequestHandler<Probe, Probe> {
        @Override
        public CompletionStage<Probe> handle(Probe request, ZLinkRouteMessageContext context) {
            return CompletableFuture.completedFuture(request);
        }
    }
}
