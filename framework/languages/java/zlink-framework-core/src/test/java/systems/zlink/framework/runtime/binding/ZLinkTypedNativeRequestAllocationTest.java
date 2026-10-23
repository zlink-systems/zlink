package systems.zlink.framework.runtime.binding;

import static org.junit.jupiter.api.Assertions.*;

import org.junit.jupiter.api.Test;

import systems.zlink.contracts.core.Context;
import systems.zlink.contracts.core.RoutingId;
import systems.zlink.contracts.core.Zlink;
import systems.zlink.contracts.messaging.Message;
import systems.zlink.framework.ZLinkEncodedPayload;
import systems.zlink.framework.ZLinkMessageSerializer;
import systems.zlink.framework.configuration.ZLinkApplicationJobQueueProfile;
import systems.zlink.framework.configuration.ZLinkMessageFlowLogMode;
import systems.zlink.framework.runtime.channels.ZLinkChannelRuntime;
import systems.zlink.framework.runtime.channels.ZLinkRequestCallerAllocationTest;
import systems.zlink.framework.runtime.configuration.DefaultZLinkFrameworkOptions;
import systems.zlink.framework.runtime.internal.backend.ZLinkMeshDispatchRecord;
import systems.zlink.framework.runtime.internal.binding.spot.MeshPeerState;
import systems.zlink.framework.runtime.internal.dispatch.ZLinkApplicationJobQueue;
import systems.zlink.framework.runtime.internal.messaging.ZLinkOwnedMessageSerializer;
import systems.zlink.framework.runtime.internal.service.ZLinkServiceSendHeaderAllocationTest;
import systems.zlink.framework.runtime.messaging.ZLinkChannelEnvelope;
import systems.zlink.framework.runtime.messaging.ZLinkJsonMessageSerializer;

import java.nio.ByteBuffer;
import java.time.Duration;
import java.util.List;
import java.util.OptionalLong;
import java.util.concurrent.CancellationException;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionException;
import java.util.concurrent.CompletionStage;
import java.util.concurrent.ExecutionException;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicInteger;

/**
 * Measures the public typed caller through actual service wire and native RouterSocket requests.
 */
public final class ZLinkTypedNativeRequestAllocationTest {
    private static final String MESH = "typed-native-allocation";
    private static final RoutingId TARGET = RoutingId.from("typed-native-allocation-target");
    private static final Duration WAIT = Duration.ofSeconds(2);
    private static final int SAMPLES = 1_000;
    private static final Request REQUEST = new Request("x".repeat(1_024));
    private static final Reply REPLY = new Reply("reply");

    public record Request(String value) {}

    public record Reply(String value) {}

    @Test
    void countsActualTypedNativeRequestsForReplyAndCancellation() throws Exception {
        ZLinkServiceSendHeaderAllocationTest.runInAllocationJvm(getClass());
    }

    public static void main(String[] args) throws Exception {
        System.out.println(
                "typed-native-request-allocation samples=1000 backend=actual-mesh "
                        + "scope=callerSubmit+callerComplete peerAllocations=excluded "
                        + "bindingJniCalls=0(binding-uses-FFM) nativeFfmCrossings=unmeasured-in-fixture "
                        + "timeoutBaseline=unmeasured(no-deterministic-native-timer) messageFlow=OFF");
        try (Fixture fixture = new Fixture()) {
            for (int index = 0; index < 100; index++) {
                fixture.roundTrip(false);
                fixture.roundTrip(true);
            }
            fixture.roundTrip(false);
            measure(fixture, false);
            measure(fixture, true);
            // A subsequent native reply also progresses after the cancelled late replies.
            fixture.roundTrip(false);
        }
    }

    private static void measure(Fixture fixture, boolean cancellation) throws Exception {
        fixture.serializer.serialized = 0;
        fixture.serializer.deserialized.set(0);
        fixture.requests.set(0);
        fixture.callbacks.set(0);
        fixture.cancelled = 0;
        var allocations =
                ZLinkRequestCallerAllocationTest.allocations(
                        Fixture.class,
                        List.of("callerSubmit", "callerComplete"),
                        () -> {
                            for (int index = 0; index < SAMPLES; index++)
                                fixture.roundTrip(cancellation);
                        });
        assertEquals(SAMPLES, fixture.serializer.serialized, "typed request serializes once");
        if (!cancellation) assertEquals(SAMPLES, fixture.serializer.deserialized.get());
        assertEquals(SAMPLES, fixture.requests.get(), "native peer receives each request once");
        assertEquals(SAMPLES, fixture.callbacks.get(), "caller observes one terminal per request");
        assertEquals(cancellation ? SAMPLES : 0, fixture.cancelled);
        assertTrue(allocations.objects() >= SAMPLES, "actual typed caller allocations are present");
        System.out.printf(
                "typed-native terminal=%s requests=%d serializerCalls=%d deserializeCalls=%d callbacks=%d "
                        + "cancelled=%d allocations=%s%n",
                cancellation ? "CANCELLATION" : "REPLY",
                fixture.requests.get(),
                fixture.serializer.serialized,
                fixture.serializer.deserialized.get(),
                fixture.callbacks.get(),
                fixture.cancelled,
                allocations);
    }

    private static final class CountingJsonSerializer
            implements ZLinkMessageSerializer, ZLinkOwnedMessageSerializer {
        final ZLinkJsonMessageSerializer json = new ZLinkJsonMessageSerializer();
        int serialized;
        final AtomicInteger deserialized = new AtomicInteger();

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
            deserialized.incrementAndGet();
            return json.deserialize(payload, type);
        }

        @Override
        public <T> T deserializeBorrowed(ByteBuffer payload, Class<T> type) {
            deserialized.incrementAndGet();
            return json.deserializeBorrowed(payload, type);
        }
    }

    private static final class Fixture implements AutoCloseable {
        final Context context = Zlink.createContext();
        final ZLinkJavaRawMeshNode source = meshNode(context);
        final ZLinkJavaRawMeshNode target = meshNode(context);
        final CountingJsonSerializer serializer = new CountingJsonSerializer();
        final ZLinkChannelRuntime runtime;
        final AtomicInteger requests = new AtomicInteger();
        final AtomicInteger callbacks = new AtomicInteger();
        volatile CompletableFuture<ZLinkMeshDispatchRecord> incoming;
        CompletionStage<Reply> terminalObservation;
        int cancelled;

        Fixture() throws Exception {
            var options = new DefaultZLinkFrameworkOptions();
            options.setDefaultRequestTimeout(WAIT);
            options.registration().dispatchOptions().messageFlow(ZLinkMessageFlowLogMode.OFF);
            runtime =
                    new ZLinkChannelRuntime(
                            new ZLinkJavaChannelBackendAdapter(),
                            options.registration(),
                            serializer);
            String endpoint = "inproc://typed-native-request-allocation-" + System.nanoTime();
            source.setRoutingId(RoutingId.from("typed-native-allocation-source"));
            source.setBind(endpoint + "-source");
            target.setRoutingId(TARGET);
            target.setBind(endpoint);
            target.startDispatch(
                    record -> {
                        requests.incrementAndGet();
                        if (!incoming.complete(record)) {
                            record.close();
                            fail("native peer dispatched an extra request");
                        }
                    });
            source.start();
            target.start();
            var admitted = new CompletableFuture<Void>();
            try (var observer =
                    source.onPeerStateChanged(
                            TARGET,
                            () -> {
                                if (source.peers().stream()
                                        .anyMatch(peer -> peer.state() == MeshPeerState.ADMITTED))
                                    admitted.complete(null);
                            },
                            () ->
                                    admitted.completeExceptionally(
                                            new IllegalStateException(
                                                    "mesh closed during admission")))) {
                source.connectPeer(endpoint, TARGET);
                admitted.get(WAIT.toMillis(), TimeUnit.MILLISECONDS);
            }
            runtime.registerSpotRouterNode(MESH, source.spotNode());
        }

        CompletableFuture<Reply> callerSubmit(boolean cancellation) {
            var future =
                    runtime.requestToNode(MESH, TARGET, REQUEST)
                            .submit(Reply.class)
                            .toCompletableFuture();
            terminalObservation =
                    future.whenComplete((reply, failure) -> callbacks.incrementAndGet());
            if (cancellation) {
                if (!future.cancel(false))
                    throw new AssertionError("native request was already terminal");
                cancelled++;
            }
            return future;
        }

        void roundTrip(boolean cancellation) {
            incoming = new CompletableFuture<>();
            CompletableFuture<Reply> future;
            Zlink.has(ZLinkServiceSendHeaderAllocationTest.NATIVE_CALLER_BOUNDARY);
            try {
                future = callerSubmit(cancellation);
            } finally {
                Zlink.has(ZLinkServiceSendHeaderAllocationTest.NATIVE_CALLER_BOUNDARY);
            }
            try (var record = incoming.get(WAIT.toMillis(), TimeUnit.MILLISECONDS)) {
                var header = ZLinkChannelEnvelope.decodeDispatchHeader(record.parts(), false);
                if (header == null || header.kind() != ZLinkChannelEnvelope.KIND_REQUEST)
                    throw new AssertionError("peer did not receive the typed request envelope");
                Message body = serializer.json.serializeOwned(REPLY);
                // The dispatch reply consumes the envelope view and closes its owned body.
                record.reply(ZLinkChannelEnvelope.encode(ZLinkChannelEnvelope.reply(header), body));
                if (!body.empty())
                    throw new AssertionError("native peer reply body was not released");
            } catch (Exception failure) {
                throw new AssertionError("native peer request/reply failed", failure);
            }
            Zlink.has(ZLinkServiceSendHeaderAllocationTest.NATIVE_CALLER_BOUNDARY);
            try {
                callerComplete(future, cancellation);
            } finally {
                Zlink.has(ZLinkServiceSendHeaderAllocationTest.NATIVE_CALLER_BOUNDARY);
            }
        }

        void callerComplete(CompletableFuture<Reply> future, boolean cancellation) {
            try {
                Reply reply =
                        terminalObservation
                                .toCompletableFuture()
                                .get(WAIT.toMillis(), TimeUnit.MILLISECONDS);
                if (cancellation || !REPLY.equals(reply))
                    throw new AssertionError("typed reply changed");
            } catch (CancellationException failure) {
                if (!cancellation) throw failure;
            } catch (ExecutionException failure) {
                Throwable cause = failure.getCause();
                while (cause instanceof CompletionException && cause.getCause() != null)
                    cause = cause.getCause();
                if (!cancellation || !(cause instanceof CancellationException))
                    throw new AssertionError("unexpected native request failure", failure);
            } catch (Exception failure) {
                throw new AssertionError("native caller terminal did not complete", failure);
            }
            if (cancellation && !future.isCancelled())
                throw new AssertionError("late native reply replaced caller cancellation");
        }

        @Override
        public void close() {
            try {
                runtime.close();
            } finally {
                source.close();
                target.close();
                context.close();
            }
        }
    }

    private static ZLinkJavaRawMeshNode meshNode(Context context) {
        var node = new ZLinkJavaRawMeshNode(context, MESH);
        node.setApplicationJobQueue(
                new ZLinkApplicationJobQueue(
                        ZLinkApplicationJobQueueProfile.BALANCED,
                        OptionalLong.empty(),
                        new ZLinkApplicationJobQueue.ProcessorCandidates(1, null, null, null)));
        return node;
    }
}
