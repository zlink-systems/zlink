package systems.zlink.framework.runtime.binding;

import static org.junit.jupiter.api.Assertions.*;

import jdk.jfr.Recording;
import jdk.jfr.consumer.RecordingFile;

import org.junit.jupiter.api.Test;

import systems.zlink.contracts.core.Context;
import systems.zlink.contracts.core.RoutingId;
import systems.zlink.contracts.messaging.Message;
import systems.zlink.contracts.messaging.RequestOperation;
import systems.zlink.contracts.messaging.RequestSubmission;
import systems.zlink.contracts.messaging.RequestSubmitOperation;
import systems.zlink.contracts.sockets.RouterSocket;
import systems.zlink.contracts.sockets.SubmitResult;
import systems.zlink.framework.runtime.internal.calls.ZLinkRequestStages;
import systems.zlink.framework.runtime.internal.execution.ZLinkStateLane;
import systems.zlink.framework.runtime.internal.service.ZLinkServiceOperationRegistry;

import java.lang.reflect.Proxy;
import java.net.URLClassLoader;
import java.nio.file.Files;
import java.nio.file.Path;
import java.time.Duration;
import java.util.LinkedHashSet;
import java.util.List;
import java.util.UUID;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicInteger;
import java.util.concurrent.atomic.AtomicReference;

/** Measures actual request adapter objects and terminals through the binding interfaces. */
public final class ZLinkJavaRequestCompletionCostTest {
    private static final int REQUESTS = 128;
    private static final Duration TIMEOUT = Duration.ofSeconds(5);

    @Test
    void registryRequestsUseOnlyBindingObserversBeforeTheirReservedCompletion() throws Exception {
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
        Path output = Files.createTempFile("zlink-request-completion-", ".log");
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
        assertAll(() -> measure(false), () -> measure(true));
    }

    /** Public test entry point for alternating original/current adapters in one JVM. */
    public static long[] benchmark(int batches, int width, boolean backpressured) throws Exception {
        try (var fixture = new Fixture(backpressured)) {
            var requests = new Submitted[width];
            long started = System.nanoTime();
            for (int batch = 0; batch < batches; batch++) {
                for (int index = 0; index < width; index++) requests[index] = fixture.submit();
                for (var request : requests) {
                    request.pending.admitted.complete(null);
                    request.pending.reply.complete(List.of(Message.from(new byte[] {2})));
                }
                for (var request : requests)
                    assertEquals(1, request.completion.get(1, TimeUnit.SECONDS));
            }
            long elapsed = System.nanoTime() - started;
            long operations = Math.multiplyExact(batches, width);
            assertEquals(operations, fixture.submits.get());
            assertEquals(operations, fixture.terminals.get());
            assertEquals(operations, fixture.decodes.get());
            assertEquals(0, fixture.discards.get());
            return new long[] {elapsed, operations, fixture.submits.get(), fixture.terminals.get()};
        }
    }

    private static void measure(boolean backpressured) throws Exception {
        Path recordingPath = Files.createTempFile("zlink-request-completion-", ".jfr");
        try (var fixture = new Fixture(backpressured)) {
            for (int i = 0; i < 32; i++) fixture.completeRequest();
            int submittedBefore = fixture.submits.get();
            int terminalsBefore = fixture.terminals.get();
            int resultsBefore = fixture.results.get();
            int admissionsBefore = fixture.admissions.get();
            try (var recording = new Recording()) {
                recording.enable("jdk.ObjectAllocationOutsideTLAB").withStackTrace();
                recording.enable("jdk.DataLoss");
                recording.start();
                for (int i = 0; i < REQUESTS; i++) fixture.completeRequest();
                recording.stop();
                recording.dump(recordingPath);
            }
            long futures = 0;
            long timers = 0;
            try (var events = new RecordingFile(recordingPath)) {
                while (events.hasMoreEvents()) {
                    var event = events.readEvent();
                    assertNotEquals("jdk.DataLoss", event.getEventType().getName());
                    if (!event.getEventType().getName().equals("jdk.ObjectAllocationOutsideTLAB"))
                        continue;
                    String allocated = event.getClass("objectClass").getName();
                    if (!(allocated.equals(CompletableFuture.class.getName())
                            || allocated.endsWith(
                                    "ScheduledThreadPoolExecutor$ScheduledFutureTask"))) continue;
                    var stack = event.getStackTrace();
                    assertNotNull(stack);
                    assertFalse(stack.isTruncated());
                    boolean portRequest = false;
                    for (var frame : stack.getFrames()) {
                        var method = frame.getMethod();
                        if (method.getType()
                                        .getName()
                                        .equals(ZLinkJavaRawServicePort.class.getName())
                                && method.getName().equals("requestMessagesTo")) portRequest = true;
                    }
                    if (!portRequest) continue;
                    if (allocated.equals(CompletableFuture.class.getName())) futures++;
                    else timers++;
                }
            }
            int submits = fixture.submits.get() - submittedBefore;
            int terminals = fixture.terminals.get() - terminalsBefore;
            int results = fixture.results.get() - resultsBefore;
            int admissions = fixture.admissions.get() - admissionsBefore;
            System.out.printf(
                    "backpressured=%s submits=%d terminals=%d results=%d admissions=%d futures=%d"
                            + " timers=%d%n",
                    backpressured, submits, terminals, results, admissions, futures, timers);
            assertEquals(REQUESTS, submits);
            assertEquals(REQUESTS, terminals);
            assertEquals(REQUESTS, results);
            assertEquals(backpressured ? REQUESTS : 0, admissions);
            // One binding reply observer, plus the admission observer only when required.
            assertEquals((backpressured ? 2L : 1L) * REQUESTS, futures);
            assertEquals(0, timers);
        } finally {
            Files.deleteIfExists(recordingPath);
        }
    }

    @Test
    void admissionFailureAndReplyFailureDeliverOneTerminalWithTheSameCause() throws Exception {
        try (var fixture = new Fixture(true)) {
            var request = fixture.submit();
            var failure = new IllegalStateException("binding admission failed");
            assertTrue(request.pending.admitted.completeExceptionally(failure));
            assertTrue(request.pending.reply.completeExceptionally(failure));
            var observed =
                    assertThrows(
                            java.util.concurrent.ExecutionException.class,
                            () -> request.completion.get(1, TimeUnit.SECONDS));
            assertSame(failure, observed.getCause());
            var admissionFailure =
                    assertThrows(
                            java.util.concurrent.ExecutionException.class,
                            () -> request.admission.get(1, TimeUnit.SECONDS));
            assertSame(failure, admissionFailure.getCause());
            assertEquals(1, fixture.terminals.get());
            assertEquals(0, fixture.decodes.get());
            assertEquals(0, fixture.operations.pendingCount());
            assertFalse(request.pending.reply.completeExceptionally(failure));
            assertEquals(1, fixture.terminals.get());
        }
    }

    @Test
    void admissionSuccessPrecedesReplyAndLateReplyIsDiscardedAfterCancellation() throws Exception {
        try (var fixture = new Fixture(true)) {
            var request = fixture.submit();
            assertFalse(request.admission.isDone());
            assertFalse(request.completion.isDone());
            request.pending.admitted.complete(null);
            assertTrue(request.admission.isDone());
            assertFalse(request.completion.isDone());
            var callerTerminals = new AtomicInteger();
            request.completion.whenComplete(
                    (ignored, failure) -> callerTerminals.incrementAndGet());
            assertTrue(request.completion.cancel(false));
            fixture.cancellationReached.get(1, TimeUnit.SECONDS);
            var late = Message.from(new byte[] {2});
            assertTrue(request.pending.reply.complete(List.of(late)));
            assertTrue(request.completion.isCancelled());
            assertEquals(1, callerTerminals.get());
            assertEquals(1, fixture.submits.get());
            assertTrue(fixture.terminals.get() <= 1);
            assertTrue(fixture.decodes.get() <= 1);
            assertEquals(fixture.decodes.get(), fixture.discards.get());
            assertEquals(0, late.size());
            assertEquals(0, fixture.operations.pendingCount());
        }
    }

    @Test
    void registryCloseWinsWithoutEndingThePendingBindingOperation() throws Exception {
        try (var fixture = new Fixture(true)) {
            var request = fixture.submit();
            fixture.operations.close();
            var failure =
                    assertThrows(
                            java.util.concurrent.ExecutionException.class,
                            () -> request.completion.get(1, TimeUnit.SECONDS));
            assertInstanceOf(IllegalStateException.class, failure.getCause());
            assertFalse(request.pending.reply.isDone());
            assertFalse(request.pending.admitted.isDone());
            request.pending.admitted.complete(null);
            var late = Message.from(new byte[] {2});
            request.pending.reply.complete(List.of(late));
            assertEquals(1, fixture.terminals.get());
            assertEquals(1, fixture.decodes.get());
            assertEquals(1, fixture.discards.get());
            assertEquals(0, late.size());
            assertEquals(0, fixture.operations.pendingCount());
        }
    }

    private record Pending(
            CompletableFuture<Void> admitted, CompletableFuture<List<Message>> reply) {}

    private record Submitted(
            Pending pending,
            CompletableFuture<Void> admission,
            CompletableFuture<Integer> completion) {}

    private static final class Fixture implements AutoCloseable {
        private final AtomicInteger submits = new AtomicInteger();
        private final AtomicInteger terminals = new AtomicInteger();
        private final AtomicInteger results = new AtomicInteger();
        private final AtomicInteger admissions = new AtomicInteger();
        private final AtomicInteger decodes = new AtomicInteger();
        private final AtomicInteger discards = new AtomicInteger();
        private final AtomicReference<Pending> pending = new AtomicReference<>();
        private final CompletableFuture<Void> cancellationReached = new CompletableFuture<>();
        private final ZLinkServiceOperationRegistry operations =
                new ZLinkServiceOperationRegistry();
        private final ZLinkJavaRawServicePort port;
        private final RouterSocket router;
        private final boolean backpressured;

        private Fixture(boolean backpressured) {
            this.backpressured = backpressured;
            router =
                    (RouterSocket)
                            Proxy.newProxyInstance(
                                    RouterSocket.class.getClassLoader(),
                                    new Class<?>[] {RouterSocket.class},
                                    (proxy, method, args) ->
                                            switch (method.getName()) {
                                                case "hashCode" -> System.identityHashCode(proxy);
                                                case "equals" -> proxy == args[0];
                                                case "setRoutingId", "close" -> null;
                                                case "request" -> operation();
                                                default ->
                                                        throw new AssertionError(method.getName());
                                            });
            var context =
                    (Context)
                            Proxy.newProxyInstance(
                                    Context.class.getClassLoader(),
                                    new Class<?>[] {Context.class},
                                    (proxy, method, args) -> {
                                        assertEquals("createRouterSocket", method.getName());
                                        return router;
                                    });
            port = new ZLinkJavaRawServicePort(context, new ZLinkStateLane(), operations);
            port.openRouter(RoutingId.from("completion-cost-source"));
        }

        private RequestOperation operation() {
            class Operation implements RequestOperation, RequestSubmitOperation {
                @Override
                public RequestSubmitOperation message(Message message) {
                    return this;
                }

                @Override
                public RequestSubmitOperation timeout(Duration timeout) {
                    return this;
                }

                @Override
                public RequestSubmission submit() {
                    submits.incrementAndGet();
                    var next = pending.get();
                    return new RequestSubmission() {
                        @Override
                        public SubmitResult result() {
                            results.incrementAndGet();
                            return backpressured ? SubmitResult.BACKPRESSURED : SubmitResult.OK;
                        }

                        @Override
                        public CompletionStage<Void> admitted() {
                            admissions.incrementAndGet();
                            return next.admitted;
                        }

                        @Override
                        public CompletionStage<List<Message>> reply() {
                            return next.reply;
                        }
                    };
                }

                @Override
                public List<Message> submit_sync() {
                    throw new AssertionError();
                }
            }
            return new Operation();
        }

        private Submitted submit() {
            var admission = new CompletableFuture<Void>();
            var next =
                    new Pending(
                            new CompletableFuture<>(),
                            new CompletableFuture<>() {
                                // This fixture keeps the binding operation active after caller
                                // cancellation.
                                @Override
                                public boolean cancel(boolean interrupt) {
                                    cancellationReached.complete(null);
                                    return false;
                                }
                            });
            pending.set(next);
            if (!backpressured) next.admitted.complete(null);
            var message = Message.from(new byte[] {1});
            CompletableFuture<Integer> completion =
                    operations.submitTerminal(
                            UUID.randomUUID(),
                            TIMEOUT,
                            terminal ->
                                    port.requestMessagesTo(
                                            router,
                                            RoutingId.from("completion-cost-target"),
                                            List.of(message),
                                            TIMEOUT,
                                            parts -> {
                                                decodes.incrementAndGet();
                                                return parts.getFirst().size();
                                            },
                                            null,
                                            (value, failure) -> {
                                                terminals.incrementAndGet();
                                                terminal.accept(value, failure);
                                            },
                                            null,
                                            admission),
                            ignored -> discards.incrementAndGet());
            var observed = ZLinkRequestStages.observe(completion, admission);
            return new Submitted(pending.get(), admission, observed.toCompletableFuture());
        }

        private void completeRequest() throws Exception {
            var request = submit();
            request.pending.admitted.complete(null);
            assertTrue(request.admission.isDone());
            request.pending.reply.complete(List.of(Message.from(new byte[] {2})));
            assertEquals(1, request.completion.get(1, TimeUnit.SECONDS));
        }

        @Override
        public void close() {
            port.close();
        }
    }
}
