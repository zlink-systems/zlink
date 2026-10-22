package systems.zlink.framework.runtime.channels;

import static org.junit.jupiter.api.Assertions.*;

import com.sun.management.ThreadMXBean;

import jdk.jfr.Recording;
import jdk.jfr.consumer.RecordingFile;

import org.junit.jupiter.api.Test;

import systems.zlink.contracts.core.RoutingId;
import systems.zlink.contracts.errors.ZlinkSubmitException;
import systems.zlink.contracts.messaging.Message;
import systems.zlink.contracts.sockets.SubmitResult;
import systems.zlink.framework.channels.ZLinkSendCall;
import systems.zlink.framework.errors.ZLinkFrameworkErrorKind;
import systems.zlink.framework.errors.ZLinkFrameworkException;
import systems.zlink.framework.runtime.configuration.DefaultZLinkFrameworkOptions;
import systems.zlink.framework.runtime.diagnostics.ZLinkMessageFlowTracer;
import systems.zlink.framework.runtime.internal.backend.*;
import systems.zlink.framework.runtime.internal.calls.ZLinkOneWayCalls;

import java.lang.management.ManagementFactory;
import java.lang.reflect.Proxy;
import java.net.URL;
import java.net.URLClassLoader;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.LinkedHashSet;
import java.util.List;
import java.util.Optional;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionException;
import java.util.concurrent.CompletionStage;

/** Exercises send-call contracts through the same package entry points as other channel tests. */
public final class ZLinkSendAdmissionAllocationTest {
    private static final String CHANNEL = "send-allocation";
    private static final RoutingId TARGET = RoutingId.from("send-allocation-target");
    private static final int SAMPLES = 100;
    private static final byte[] PAYLOAD = new byte[1024];
    private static volatile CompletionStage<Void> observed;

    @Test
    void immediateSendHasNoCompletionGraphAllocations() throws Exception {
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
        Path output = Files.createTempFile("zlink-send-allocation-", ".log");
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

    @Test
    void completionAndPayloadLifetimeRemainIndependentOfCallerCancellation() {
        for (Kind kind : Kind.values()) {
            try (Fixture fixture = new Fixture(kind)) {
                fixture.submission = ZLinkOneWayCalls.immediateAdmission();
                var immediate = fixture.send();
                assertTrue(fixture.parts.stream().allMatch(Message::empty), kind.name());
                var first = immediate.toCompletableFuture();
                var second = immediate.toCompletableFuture();
                assertNotSame(first, second);
                first.obtrudeException(new IllegalStateException("caller change"));
                second.join();

                fixture.submission = CompletableFuture.completedFuture(null);
                assertFalse(ZLinkOneWayCalls.isImmediateAdmission(fixture.send()));
                assertTrue(fixture.parts.stream().allMatch(Message::empty));

                fixture.submission =
                        CompletableFuture.failedFuture(
                                new ZlinkSubmitException(SubmitResult.NOT_FOUND));
                assertUnavailable(fixture.send());
                assertTrue(fixture.parts.stream().allMatch(Message::empty));

                for (int terminal = 0; terminal < 4; terminal++) {
                    var pending = new CompletableFuture<Void>();
                    fixture.submission = pending;
                    var caller = fixture.send().toCompletableFuture();
                    assertFalse(
                            fixture.parts.getLast().empty(), "pending send retains its payload");
                    assertFalse(caller.isDone());
                    if (terminal == 0) {
                        pending.complete(null);
                        caller.join();
                    } else if (terminal == 1) {
                        pending.completeExceptionally(
                                new ZlinkSubmitException(SubmitResult.NOT_FOUND));
                        assertUnavailable(caller);
                    } else if (terminal == 2) {
                        assertTrue(caller.cancel(false));
                        assertFalse(
                                pending.isDone(),
                                "caller cancellation does not revoke binding admission");
                        assertFalse(fixture.parts.getLast().empty());
                        pending.complete(null);
                    } else {
                        pending.cancel(false);
                        Throwable cancelled = assertThrows(RuntimeException.class, caller::join);
                        while (cancelled instanceof CompletionException
                                && cancelled.getCause() != null) {
                            cancelled = cancelled.getCause();
                        }
                        assertInstanceOf(
                                java.util.concurrent.CancellationException.class, cancelled);
                    }
                    assertTrue(fixture.parts.stream().allMatch(Message::empty), kind.name());
                }
            }
        }
    }

    @Test
    void callerCancellationDoesNotCancelPayloadRelease() {
        for (Kind kind : Kind.values()) {
            try (Fixture fixture = new Fixture(kind)) {
                var binding = new CompletableFuture<Void>();
                fixture.submission = binding;
                var caller = fixture.send().toCompletableFuture();
                assertTrue(caller.cancel(false));
                assertFalse(binding.isDone());
                assertFalse(fixture.parts.getLast().empty());
                binding.complete(null);
                boolean closed = fixture.parts.stream().allMatch(Message::empty);
                System.out.printf("caller-cancel kind=%s payloadsClosed=%s%n", kind, closed);
                try {
                    assertTrue(closed, kind.name());
                } finally {
                    Message.closeAll(fixture.parts);
                }
            }
        }
    }

    @Test
    void localNodeCallerCancellationDoesNotCancelPayloadRelease() {
        try (Fixture fixture = new Fixture(Kind.MESH_NODE, true)) {
            var admission = new CompletableFuture<Integer>();
            fixture.localSubmission = admission;
            var caller = fixture.send().toCompletableFuture();
            assertTrue(caller.cancel(false));
            assertFalse(admission.isDone());
            assertFalse(fixture.parts.getLast().empty());
            admission.complete(ZLinkOneWayCalls.SUBMITTED);
            try {
                assertTrue(fixture.parts.stream().allMatch(Message::empty));
            } finally {
                Message.closeAll(fixture.parts);
            }
            fixture.localSubmission =
                    CompletableFuture.completedFuture(ZLinkOneWayCalls.ROUTE_NOT_CONNECTED);
            assertUnavailable(fixture.send());
            assertTrue(fixture.parts.stream().allMatch(Message::empty));
        }
    }

    private static void assertUnavailable(CompletionStage<Void> completion) {
        var failure =
                assertThrows(
                        CompletionException.class, () -> completion.toCompletableFuture().join());
        assertEquals(
                ZLinkFrameworkErrorKind.UNAVAILABLE,
                assertInstanceOf(ZLinkFrameworkException.class, failure.getCause()).kind());
    }

    public static void main(String[] args) throws Exception {
        if (args.length == 2 && args[0].equals("--benchmark")) {
            benchmark(Path.of(args[1]));
            return;
        }
        long totalImmediate = 0;
        for (Kind kind : Kind.values()) {
            try (Fixture fixture = new Fixture(kind)) {
                for (int warmup = 0; warmup < 500; warmup++) fixture.send();
                long immediate =
                        countCompletionObjects(fixture, ZLinkOneWayCalls.immediateAdmission());
                long ordinary =
                        countCompletionObjects(fixture, CompletableFuture.completedFuture(null));
                System.out.printf(
                        "%s immediateCompletionObjects=%d ordinaryCompletionObjects=%d sends=%d%n",
                        kind, immediate, ordinary, SAMPLES);
                totalImmediate += immediate;
                assertTrue(ordinary > 0, "ordinary completed stages keep the error adapter");
            }
        }
        assertEquals(0, totalImmediate, "terminal binding admission needs no completion graph");
        var contracts = new ZLinkSendAdmissionAllocationTest();
        contracts.completionAndPayloadLifetimeRemainIndependentOfCallerCancellation();
        contracts.callerCancellationDoesNotCancelPayloadRelease();
        contracts.localNodeCallerCancellationDoesNotCancelPayloadRelease();
        System.out.println("send completion and payload contracts passed");
    }

    /** Runs original and current public send paths in isolated loaders in the same JVM. */
    private static void benchmark(Path baselineClasses) throws Exception {
        var paths = new ArrayList<String>();
        for (Kind kind : Kind.values()) paths.add(kind.name());
        for (Kind kind : Kind.values()) paths.add(kind.name() + ":pending");
        paths.add("MESH_NODE:local-pending");
        benchmarkPaths(baselineClasses, ZLinkSendAdmissionAllocationTest.class, paths, 100_000);
    }

    public static void benchmarkPaths(
            Path baselineClasses, Class<?> fixtureClass, List<String> paths, int count)
            throws Exception {
        var current = new ArrayList<URL>();
        for (String entry :
                System.getProperty("java.class.path").split(java.io.File.pathSeparator)) {
            current.add(Path.of(entry).toUri().toURL());
        }
        var original = new ArrayList<URL>();
        original.add(baselineClasses.toUri().toURL());
        original.addAll(current);
        try (var before = new FrameworkLoader(original.toArray(URL[]::new));
                var after = new FrameworkLoader(current.toArray(URL[]::new));
                var control = new FrameworkLoader(current.toArray(URL[]::new))) {
            var methods =
                    List.of(before, after, control).stream()
                            .map(
                                    loader -> {
                                        try {
                                            return loader.loadClass(fixtureClass.getName())
                                                    .getMethod(
                                                            "measureSendPath",
                                                            String.class,
                                                            int.class);
                                        } catch (ReflectiveOperationException failure) {
                                            throw new IllegalStateException(failure);
                                        }
                                    })
                            .toList();
            for (String path : paths) {
                for (int warmup = 0; warmup < 8; warmup++) {
                    for (var method : methods) method.invoke(null, path, count);
                }
                double[][][] values = new double[3][3][10];
                for (int round = 0; round < 10; round++) {
                    for (int step = 0; step < 3; step++) {
                        int variant = (round & 1) == 0 ? step : 2 - step;
                        long[] result = (long[]) methods.get(variant).invoke(null, path, count);
                        for (int metric = 0; metric < 3; metric++) {
                            values[variant][metric][round] = result[metric] / (double) count;
                        }
                    }
                    System.out.printf(
                            "raw path=%s round=%d A_ns=%.2f B_ns=%.2f C_ns=%.2f A_bytes=%.2f B_bytes=%.2f C_bytes=%.2f A_cpu=%.2f B_cpu=%.2f C_cpu=%.2f%n",
                            path,
                            round,
                            values[0][0][round],
                            values[1][0][round],
                            values[2][0][round],
                            values[0][1][round],
                            values[1][1][round],
                            values[2][1][round],
                            values[0][2][round],
                            values[1][2][round],
                            values[2][2][round]);
                }
                double[][] medians = new double[3][3];
                for (int variant = 0; variant < 3; variant++) {
                    for (int metric = 0; metric < 3; metric++) {
                        Arrays.sort(values[variant][metric]);
                        medians[variant][metric] =
                                (values[variant][metric][4] + values[variant][metric][5]) / 2;
                    }
                }
                System.out.printf(
                        "median path=%s A_ns=%.2f B_ns=%.2f C_ns=%.2f A_bytes=%.2f B_bytes=%.2f C_bytes=%.2f A_cpu=%.2f B_cpu=%.2f C_cpu=%.2f%n",
                        path,
                        medians[0][0],
                        medians[1][0],
                        medians[2][0],
                        medians[0][1],
                        medians[1][1],
                        medians[2][1],
                        medians[0][2],
                        medians[1][2],
                        medians[2][2]);
            }
        }
    }

    /** Shares the binding/native library while selecting the original or current Framework. */
    private static final class FrameworkLoader extends URLClassLoader {
        FrameworkLoader(URL[] urls) {
            super(urls, ZLinkSendAdmissionAllocationTest.class.getClassLoader());
        }

        @Override
        protected Class<?> loadClass(String name, boolean resolve) throws ClassNotFoundException {
            synchronized (getClassLoadingLock(name)) {
                if (!name.startsWith("systems.zlink.framework."))
                    return super.loadClass(name, resolve);
                Class<?> loaded = findLoadedClass(name);
                if (loaded == null) loaded = findClass(name);
                if (resolve) resolveClass(loaded);
                return loaded;
            }
        }
    }

    /** Public benchmark boundary, avoiding private access and rewritten production bytecode. */
    public static long[] measureSendPath(String path, int count) {
        ThreadMXBean allocations = (ThreadMXBean) ManagementFactory.getThreadMXBean();
        String[] components = path.split(":");
        boolean pending = components.length > 1;
        boolean local = pending && components[1].equals("local-pending");
        try (Fixture fixture = new Fixture(Kind.valueOf(components[0]), local)) {
            long before = allocations.getCurrentThreadAllocatedBytes();
            long cpuStart = allocations.getCurrentThreadCpuTime();
            long start = System.nanoTime();
            for (int sample = 0; sample < count; sample++) {
                if (local) {
                    var admission = new CompletableFuture<Integer>();
                    fixture.localSubmission = admission;
                    observed = fixture.send();
                    admission.complete(ZLinkOneWayCalls.SUBMITTED);
                } else if (pending) {
                    var admission = new CompletableFuture<Void>();
                    fixture.submission = admission;
                    observed = fixture.send();
                    admission.complete(null);
                } else {
                    observed = fixture.send();
                }
            }
            long elapsed = System.nanoTime() - start;
            long cpu = allocations.getCurrentThreadCpuTime() - cpuStart;
            return new long[] {elapsed, allocations.getCurrentThreadAllocatedBytes() - before, cpu};
        }
    }

    private static long countCompletionObjects(Fixture fixture, CompletionStage<Void> submission)
            throws Exception {
        fixture.submission = submission;
        Path file = Files.createTempFile("zlink-send-", ".jfr");
        try {
            try (Recording recording = new Recording()) {
                recording.enable("jdk.ObjectAllocationOutsideTLAB").withStackTrace();
                recording.enable("jdk.DataLoss");
                recording.start();
                for (int sample = 0; sample < SAMPLES; sample++) observed = fixture.send();
                recording.stop();
                recording.dump(file);
            }
            long count = 0;
            try (var events = new RecordingFile(file)) {
                while (events.hasMoreEvents()) {
                    var event = events.readEvent();
                    assertNotEquals("jdk.DataLoss", event.getEventType().getName());
                    if (!event.getEventType().getName().equals("jdk.ObjectAllocationOutsideTLAB"))
                        continue;
                    if (!event.getClass("objectClass")
                            .getName()
                            .startsWith("java.util.concurrent.CompletableFuture")) continue;
                    if (event.getStackTrace() != null
                            && event.getStackTrace().getFrames().stream()
                                    .anyMatch(
                                            frame ->
                                                    frame.getMethod()
                                                                    .getType()
                                                                    .getName()
                                                                    .equals(Fixture.class.getName())
                                                            && frame.getMethod()
                                                                    .getName()
                                                                    .equals("send"))) count++;
                }
            }
            return count;
        } finally {
            Files.deleteIfExists(file);
        }
    }

    enum Kind {
        CLIENT,
        MESH_CHANNEL,
        ROUTER,
        MESH_NODE
    }

    static final class Fixture implements AutoCloseable {
        final Kind kind;
        final ZLinkChannelSocketRegistry sockets = new ZLinkChannelSocketRegistry();
        final ZLinkChannelCallRuntime runtime;
        CompletionStage<Void> submission = ZLinkOneWayCalls.immediateAdmission();
        List<Message> parts = List.of();
        CompletionStage<Integer> localSubmission =
                CompletableFuture.completedFuture(ZLinkOneWayCalls.SUBMITTED);

        Fixture(Kind kind) {
            this(kind, false);
        }

        Fixture(Kind kind, boolean local) {
            this.kind = kind;
            var options = new DefaultZLinkFrameworkOptions();
            runtime =
                    new ZLinkChannelCallRuntime(
                            new ZLinkMessageFlowTracer(
                                    options.registration().dispatchOptions(), null, Runnable::run),
                            null,
                            null,
                            null);
            Class<?> backend =
                    switch (kind) {
                        case CLIENT -> ZLinkBackendDealerSocket.class;
                        case ROUTER -> ZLinkBackendRouterSocket.class;
                        default -> ZLinkInternalSpotNode.class;
                    };
            Object socket =
                    Proxy.newProxyInstance(
                            getClass().getClassLoader(),
                            new Class<?>[] {backend},
                            (proxy, method, args) -> {
                                return switch (method.getName()) {
                                    case "send", "sendToChannel", "sendToNode" -> {
                                        @SuppressWarnings("unchecked")
                                        List<Message> sent = (List<Message>) args[args.length - 1];
                                        parts = sent;
                                        yield submission;
                                    }
                                    case "routingId" ->
                                            local ? TARGET : RoutingId.from("other-node");
                                    case "submitLocalNodeSend" -> {
                                        @SuppressWarnings("unchecked")
                                        List<Message> sent = (List<Message>) args[args.length - 1];
                                        parts = sent;
                                        yield Optional.of(localSubmission);
                                    }
                                    case "classifyNodeSendTarget" -> Optional.empty();
                                    default -> null;
                                };
                            });
            switch (kind) {
                case CLIENT -> {
                    var registration = new ChannelRegistration(CHANNEL, ChannelKind.CLIENT_SERVER);
                    registration.enableClient();
                    sockets.registerChannel(registration);
                    sockets.registerClient(CHANNEL, (ZLinkBackendDealerSocket) socket);
                    sockets.enableUnmanagedBackendClientMode();
                }
                case ROUTER ->
                        sockets.registerRouteRouter(CHANNEL, (ZLinkBackendRouterSocket) socket);
                default -> sockets.registerSpotRouterNode(CHANNEL, (ZLinkInternalSpotNode) socket);
            }
        }

        CompletionStage<Void> send() {
            Message payload = Message.from(PAYLOAD);
            ZLinkSendCall call =
                    switch (kind) {
                        case CLIENT, MESH_CHANNEL ->
                                new ChannelSendCall(runtime, CHANNEL, sockets, payload, null);
                        default ->
                                new RouteSendCall(
                                        runtime,
                                        CHANNEL,
                                        sockets,
                                        TARGET,
                                        payload,
                                        null,
                                        ZLinkChannelContentTypeFrame.DEFAULT_CONTENT_TYPE,
                                        null);
                    };
            return call.submit();
        }

        @Override
        public void close() {
            runtime.beginClose();
            sockets.closeAll();
            Message.closeAll(parts);
        }
    }
}
