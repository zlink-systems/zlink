package systems.zlink.framework.runtime.channels;

import static org.junit.jupiter.api.Assertions.*;

import jdk.jfr.Recording;
import jdk.jfr.consumer.RecordingFile;

import org.junit.jupiter.api.Test;

import systems.zlink.contracts.core.RoutingId;
import systems.zlink.contracts.core.Zlink;
import systems.zlink.contracts.errors.ZlinkRequestException;
import systems.zlink.contracts.messaging.Message;
import systems.zlink.contracts.sockets.RequestResult;
import systems.zlink.framework.ZLinkEncodedPayload;
import systems.zlink.framework.ZLinkMessageSerializer;
import systems.zlink.framework.configuration.ZLinkMessageFlowLogMode;
import systems.zlink.framework.errors.ZLinkFrameworkErrorKind;
import systems.zlink.framework.errors.ZLinkFrameworkException;
import systems.zlink.framework.runtime.configuration.DefaultZLinkFrameworkOptions;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendContext;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendReceived;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendRequestResult;
import systems.zlink.framework.runtime.internal.backend.ZLinkChannelBackendAdapter;
import systems.zlink.framework.runtime.internal.backend.ZLinkInternalSpotNode;
import systems.zlink.framework.runtime.internal.messaging.ZLinkOwnedMessageSerializer;
import systems.zlink.framework.runtime.internal.service.ZLinkFrameworkMultipartView;
import systems.zlink.framework.runtime.internal.service.ZLinkServiceM6AWireCodec;
import systems.zlink.framework.runtime.internal.service.ZLinkServiceSendHeaderAllocationTest;
import systems.zlink.framework.runtime.messaging.ZLinkJsonMessageSerializer;

import java.lang.reflect.Proxy;
import java.nio.ByteBuffer;
import java.nio.file.Files;
import java.nio.file.Path;
import java.time.Duration;
import java.util.List;
import java.util.Optional;
import java.util.concurrent.CancellationException;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionException;
import java.util.concurrent.CompletionStage;

/** Counts the public typed request caller against the same fake backend without its caller. */
public final class ZLinkRequestCallerAllocationTest {
    private static final String MESH = "request-allocation";
    private static final RoutingId TARGET = RoutingId.from("request-allocation-target");
    private static final Duration TIMEOUT = Duration.ofSeconds(10);
    private static final int SAMPLES = 1_000;
    private static final Request REQUEST = new Request("x".repeat(1_024));
    private static final Reply REPLY = new Reply("reply");
    private static volatile Object observed;

    public record Request(String value) {}

    public record Reply(String value) {}

    @Test
    void countsOneThousandPublicTypedRequestsForEachTerminal() throws Exception {
        ZLinkServiceSendHeaderAllocationTest.runInAllocationJvm(getClass());
    }

    public static void main(String[] args) throws Exception {
        System.out.println(
                "request-allocation scope=public-typed-caller fakeBackendBaseline=direct"
                        + " actualBindingBaseline=separate-ZLinkBindingRequestAllocationTest"
                        + " bindingJniCalls=0(binding-uses-FFM) nativeFfmCrossings=unmeasured"
                        + " allocationEvent=OutsideTLAB UseTLAB=false DoEscapeAnalysis=false"
                        + " messageFlow=OFF");
        for (Terminal terminal : Terminal.values()) {
            try (Fixture fixture = new Fixture(terminal)) {
                for (int index = 0; index < 300; index++) {
                    fixture.sample(false);
                    fixture.sample(true);
                }
                AllocationCount baseline = measure(fixture, false);
                AllocationCount caller = measure(fixture, true);
                System.out.printf(
                        "terminal=%s samples=%d mock=%s public=%s deltaBytes=%d deltaObjects=%d "
                                + "deltaContinuations=%d%n",
                        terminal,
                        SAMPLES,
                        baseline,
                        caller,
                        caller.bytes - baseline.bytes,
                        caller.objects - baseline.objects,
                        caller.continuations - baseline.continuations);
                assertTrue(
                        baseline.objects >= SAMPLES,
                        "negative control observes fake backend futures");
                assertTrue(caller.objects > baseline.objects, "public caller costs are observable");
            }
        }
    }

    private static AllocationCount measure(Fixture fixture, boolean publicCaller) throws Exception {
        fixture.resetCounts();
        AllocationCount count =
                allocations(
                        Fixture.class,
                        List.of("sample"),
                        () -> {
                            for (int index = 0; index < SAMPLES; index++)
                                fixture.sample(publicCaller);
                        });
        fixture.assertCounts(publicCaller);
        return count;
    }

    /** Shared allocation recorder; only the supplied caller stack boundaries contribute. */
    public static AllocationCount allocations(
            Class<?> owner, List<String> methods, Runnable samples) throws Exception {
        Path path = Files.createTempFile("zlink-request-caller-", ".jfr");
        try {
            try (Recording recording = new Recording()) {
                recording.enable("jdk.ObjectAllocationOutsideTLAB").withStackTrace();
                recording.enable("jdk.DataLoss");
                recording.start();
                Zlink.has(ZLinkServiceSendHeaderAllocationTest.NATIVE_COUNTER_BOUNDARY);
                samples.run();
                Zlink.has(ZLinkServiceSendHeaderAllocationTest.NATIVE_COUNTER_BOUNDARY);
                recording.stop();
                recording.dump(path);
            }
            long objects = 0, bytes = 0, byteArrays = 0, futures = 0, continuations = 0;
            try (RecordingFile events = new RecordingFile(path)) {
                while (events.hasMoreEvents()) {
                    var event = events.readEvent();
                    assertNotEquals("jdk.DataLoss", event.getEventType().getName());
                    if (!event.getEventType().getName().equals("jdk.ObjectAllocationOutsideTLAB"))
                        continue;
                    if (event.getStackTrace() == null
                            || event.getStackTrace().getFrames().stream()
                                    .noneMatch(
                                            frame ->
                                                    frame.getMethod()
                                                                    .getType()
                                                                    .getName()
                                                                    .equals(owner.getName())
                                                            && methods.contains(
                                                                    frame.getMethod().getName())))
                        continue;
                    String type = event.getClass("objectClass").getName();
                    objects++;
                    bytes += event.getLong("allocationSize");
                    if (type.equals("[B")) byteArrays++;
                    if (type.equals("java.util.concurrent.CompletableFuture")) futures++;
                    if (type.startsWith("java.util.concurrent.CompletableFuture$")) continuations++;
                }
            }
            return new AllocationCount(objects, bytes, byteArrays, futures, continuations);
        } finally {
            Files.deleteIfExists(path);
        }
    }

    public record AllocationCount(
            long objects, long bytes, long byteArrays, long futures, long continuations) {
        @Override
        public String toString() {
            return "{bytes="
                    + bytes
                    + ",objects="
                    + objects
                    + ",byteArrays="
                    + byteArrays
                    + ",futures="
                    + futures
                    + ",continuations="
                    + continuations
                    + "}";
        }
    }

    private enum Terminal {
        REPLY,
        CANCELLATION,
        TIMEOUT
    }

    private static final class CountingSerializer
            implements ZLinkMessageSerializer, ZLinkOwnedMessageSerializer {
        final ZLinkJsonMessageSerializer json = new ZLinkJsonMessageSerializer();
        int serialized;
        int deserialized;

        @Override
        public <T> ZLinkEncodedPayload serialize(T value) {
            serialized++;
            return json.serialize(value);
        }

        @Override
        public Message serializeOwned(Object value) {
            serialized++;
            return json.serializeOwned(value);
        }

        @Override
        public <T> T deserialize(ZLinkEncodedPayload payload, Class<T> type) {
            deserialized++;
            return json.deserialize(payload, type);
        }

        @Override
        public <T> T deserializeBorrowed(ByteBuffer payload, Class<T> type) {
            deserialized++;
            return json.deserializeBorrowed(payload, type);
        }
    }

    private static final class Fixture implements AutoCloseable {
        final Terminal terminal;
        final CountingSerializer serializer = new CountingSerializer();
        final ZLinkChannelRuntime runtime;
        final List<Message> baselineParts;
        final Message replyBody;
        CompletableFuture<ZLinkBackendReceived> bindingReply;
        Message borrowedBody;
        Message retained;
        int mockRequests;
        int terminalCallbacks;
        int payloadReleases;
        int replyReleases;
        int cancellations;

        Fixture(Terminal terminal) {
            this.terminal = terminal;
            baselineParts =
                    List.of(Message.from("header"), serializer.json.serializeOwned(REQUEST));
            replyBody = serializer.json.serializeOwned(REPLY);
            ZLinkBackendContext context =
                    new ZLinkBackendContext() {
                        @Override
                        public String name() {
                            return "request-allocation-context";
                        }

                        @Override
                        public void shutdown() {}

                        @Override
                        public void close() {}
                    };
            var backend =
                    (ZLinkChannelBackendAdapter)
                            Proxy.newProxyInstance(
                                    getClass().getClassLoader(),
                                    new Class<?>[] {ZLinkChannelBackendAdapter.class},
                                    (proxy, method, arguments) -> {
                                        if (method.getName().equals("createContext"))
                                            return context;
                                        throw new AssertionError(
                                                "unexpected backend factory: " + method.getName());
                                    });
            var options = new DefaultZLinkFrameworkOptions();
            options.registration().dispatchOptions().messageFlow(ZLinkMessageFlowLogMode.OFF);
            runtime = new ZLinkChannelRuntime(backend, options.registration(), serializer);
            var node =
                    (ZLinkInternalSpotNode)
                            Proxy.newProxyInstance(
                                    getClass().getClassLoader(),
                                    new Class<?>[] {ZLinkInternalSpotNode.class},
                                    (proxy, method, arguments) -> {
                                        return switch (method.getName()) {
                                            case "requestToNode" -> {
                                                @SuppressWarnings("unchecked")
                                                List<Message> parts = (List<Message>) arguments[2];
                                                @SuppressWarnings("unchecked")
                                                CompletableFuture<Void> admission =
                                                        (CompletableFuture<Void>) arguments[6];
                                                admission.complete(null);
                                                yield request(parts);
                                            }
                                            case "classifyNodeSendTarget" -> Optional.empty();
                                            case "name" -> MESH;
                                            case "sourceTransportLane" -> null;
                                            case "close" -> null;
                                            default ->
                                                    throw new AssertionError(
                                                            "unexpected node call: "
                                                                    + method.getName());
                                        };
                                    });
            runtime.registerSpotRouterNode(MESH, node);
        }

        void resetCounts() {
            serializer.serialized = 0;
            serializer.deserialized = 0;
            mockRequests = terminalCallbacks = payloadReleases = replyReleases = cancellations = 0;
        }

        CompletionStage<ZLinkBackendReceived> request(List<Message> parts) {
            mockRequests++;
            borrowedBody = parts.getLast();
            retained = ZLinkServiceM6AWireCodec.encodeFrameworkMultipartMessage(parts);
            bindingReply =
                    terminal == Terminal.TIMEOUT
                            ? CompletableFuture.failedFuture(
                                    new ZlinkRequestException(RequestResult.TIMED_OUT))
                            : new CompletableFuture<>();
            Message owned = retained;
            bindingReply.whenComplete(
                    (reply, failure) -> {
                        if (bindingReply.isCancelled()) cancellations++;
                        owned.close();
                        payloadReleases++;
                    });
            return bindingReply;
        }

        ZLinkBackendReceived reply() {
            return new ZLinkBackendReceived(
                    ZLinkBackendRequestResult.OK,
                    Optional.empty(),
                    Optional.empty(),
                    Optional.empty(),
                    new byte[0],
                    new byte[0],
                    List.of(Message.from(replyBody)),
                    null,
                    () -> replyReleases++,
                    null);
        }

        void sample(boolean publicCaller) {
            CompletableFuture<?> caller =
                    publicCaller
                            ? runtime.requestToNode(MESH, TARGET, REQUEST)
                                    .submit(Reply.class)
                                    .toCompletableFuture()
                            : request(baselineParts).toCompletableFuture();
            caller.whenComplete((value, failure) -> terminalCallbacks++);
            if (terminal == Terminal.REPLY) bindingReply.complete(reply());
            else if (terminal == Terminal.CANCELLATION) {
                if (!caller.cancel(false) || !bindingReply.isCancelled())
                    throw new AssertionError(
                            "public caller cancellation must reach its backend waiter");
                ZLinkBackendReceived late = reply();
                if (bindingReply.complete(late))
                    throw new AssertionError("late completion changed cancellation");
                late.close();
            }
            try {
                Object result = caller.join();
                if (terminal != Terminal.REPLY)
                    throw new AssertionError("missing terminal failure");
                if (publicCaller) {
                    if (!REPLY.equals(result)) throw new AssertionError("typed reply changed");
                } else ((ZLinkBackendReceived) result).close();
                observed = result;
            } catch (CancellationException failure) {
                if (terminal != Terminal.CANCELLATION) throw failure;
            } catch (CompletionException failure) {
                if (terminal == Terminal.CANCELLATION) {
                    if (!(failure.getCause() instanceof CancellationException)) throw failure;
                } else if (terminal == Terminal.TIMEOUT) {
                    if (publicCaller) {
                        if (!(failure.getCause() instanceof ZLinkFrameworkException framework)
                                || framework.kind() != ZLinkFrameworkErrorKind.DEADLINE_EXCEEDED)
                            throw failure;
                    } else if (!(failure.getCause() instanceof ZlinkRequestException request)
                            || request.getResult() != RequestResult.TIMED_OUT) throw failure;
                } else throw failure;
            }
            if (!retained.empty()) throw new AssertionError("backend payload leaked");
            if (publicCaller && !borrowedBody.empty())
                throw new AssertionError("caller payload leaked");
        }

        void assertCounts(boolean publicCaller) {
            assertEquals(SAMPLES, mockRequests);
            assertEquals(SAMPLES, terminalCallbacks, "one caller terminal callback per operation");
            assertEquals(
                    SAMPLES, payloadReleases, "backend payload is released for every terminal");
            assertEquals(
                    terminal == Terminal.TIMEOUT ? 0 : SAMPLES,
                    replyReleases,
                    "successful and rejected late replies each release their payload");
            assertEquals(terminal == Terminal.CANCELLATION ? SAMPLES : 0, cancellations);
            assertEquals(
                    publicCaller ? SAMPLES : 0,
                    serializer.serialized,
                    "typed payload serializes once");
            assertEquals(
                    publicCaller && terminal == Terminal.REPLY ? SAMPLES : 0,
                    serializer.deserialized);
        }

        @Override
        public void close() {
            runtime.close();
            ZLinkFrameworkMultipartView.closeAll(baselineParts);
            replyBody.close();
        }
    }
}
