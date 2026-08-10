package systems.zlink.framework.runtime.spots;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertNotSame;
import static org.junit.jupiter.api.Assertions.assertSame;
import static org.junit.jupiter.api.Assertions.assertThrows;
import static org.junit.jupiter.api.Assertions.assertTrue;

import com.fasterxml.jackson.databind.JsonNode;
import com.fasterxml.jackson.databind.ObjectMapper;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.List;
import java.util.Map;
import java.util.Optional;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.ExecutionException;
import java.util.concurrent.Executors;
import java.util.concurrent.Future;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicInteger;
import org.junit.jupiter.api.Test;
import systems.zlink.contracts.messaging.Message;
import systems.zlink.framework.ZLinkEncodedPayload;
import systems.zlink.framework.ZLinkMessageSerializer;
import systems.zlink.framework.errors.ZLinkFrameworkErrorKind;
import systems.zlink.framework.errors.ZLinkFrameworkException;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendReceived;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendRequestResult;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendTopicMessage;
import systems.zlink.framework.spots.ZLinkSpotSubscriptionHandler;

final class ZLinkPayloadOwnershipConformanceTest {
    @Test
    void consumesSharedOwnershipAndCopyBudget() throws Exception {
        JsonNode fixture = fixture();
        assertEquals("zlink.framework.payload-ownership", fixture.path("fixture").asText());
        assertEquals(1, fixture.path("version").asInt());
        List<String> ownershipStates = new java.util.ArrayList<>();
        fixture.path("ownershipStates").forEach(
            state -> ownershipStates.add(state.asText()));
        assertEquals(
            List.of("bindingBorrowed", "frameworkOwned", "applicationBorrowed", "released"),
            ownershipStates);
        JsonNode copyBudget = fixture.path("copyBudget");
        assertEquals(1, copyBudget.path("bindingWithoutOwnershipHandoff").asInt());
        assertEquals(0, copyBudget.path("frameworkCopiesAfterOwnership").asInt());
        assertEquals(0, copyBudget.path("readonlyAccessorCopies").asInt());
        assertEquals(1, copyBudget.path("maximumDeserializationsAfterAdmission").asInt());
        for (JsonNode scenario : fixture.path("scenarios")) {
            assertTrue(
                scenario.path("deserializations").asInt() <= 1,
                scenario.path("name").asText());
            assertTrue(
                scenario.path("frameworkReleases").asInt() <= 1,
                scenario.path("name").asText());
        }
    }

    @Test
    void metadataCopiesOnlyAtAdmissionAndAccessorsReturnOwnerViews() {
        byte[] routeSource = new byte[] {7, 8};
        ZLinkBackendReceived route = new ZLinkBackendReceived(
            ZLinkBackendRequestResult.OK,
            Optional.empty(),
            Optional.empty(),
            Optional.empty(),
            routeSource,
            List.of(Message.from("route")),
            null,
            () -> { });
        byte[] topicSource = new byte[] {9, 10};
        ZLinkBackendTopicMessage topic = new ZLinkBackendTopicMessage(
            Optional.empty(),
            "channel",
            "topic",
            topicSource,
            List.of(Message.from("topic")));
        try {
            routeSource[0] = 1;
            topicSource[0] = 2;
            byte[] routeView = route.applicationMetadata();
            byte[] topicView = topic.applicationMetadata();
            assertNotSame(routeSource, routeView);
            assertNotSame(topicSource, topicView);
            assertEquals(7, routeView[0]);
            assertEquals(9, topicView[0]);
            assertSame(routeView, route.applicationMetadata());
            assertSame(routeView, route.applicationMetadata());
            assertSame(topicView, topic.applicationMetadata());
            assertSame(topicView, topic.applicationMetadata());
        } finally {
            route.close();
            topic.parts().forEach(Message::close);
        }
    }

    @Test
    void rawViewsDoNotConsumeTheSingleTypedDecodeOutcome() {
        Probe decoded = new Probe("decoded");
        CountingSerializer serializer = new CountingSerializer(decoded, null);
        try (Message payload = Message.from("payload")) {
            ZLinkInboundPayloadOwner owner = new ZLinkInboundPayloadOwner(payload, serializer);
            assertSame(payload, owner.rawView());
            assertSame(payload, owner.rawView());
            assertSame(decoded, owner.deserialize(Probe.class));
            assertSame(decoded, owner.deserialize(Probe.class));
            assertEquals(1, serializer.calls.get());
        }
    }

    @Test
    void concurrentAndDifferentTypedAccessorsNeverDecodeTwice() throws Exception {
        Probe decoded = new Probe("decoded");
        CountDownLatch decoderEntered = new CountDownLatch(1);
        CountDownLatch releaseDecoder = new CountDownLatch(1);
        CountingSerializer serializer = new CountingSerializer(
            decoded, null, decoderEntered, releaseDecoder);
        try (Message payload = Message.from("payload")) {
            ZLinkInboundPayloadOwner owner = new ZLinkInboundPayloadOwner(payload, serializer);
            var executor = Executors.newFixedThreadPool(12);
            try {
                CountDownLatch start = new CountDownLatch(1);
                List<Future<Object>> results = java.util.stream.IntStream.range(0, 24)
                    .mapToObj(ignored -> executor.submit(() -> {
                        start.await();
                        return owner.deserialize(Probe.class);
                    }))
                    .toList();
                start.countDown();
                assertTrue(decoderEntered.await(5, TimeUnit.SECONDS));
                releaseDecoder.countDown();
                for (Future<Object> result : results) {
                    assertSame(decoded, result.get(5, TimeUnit.SECONDS));
                }
            } finally {
                releaseDecoder.countDown();
                executor.shutdownNow();
            }

            ZLinkFrameworkException mismatch = assertThrows(
                ZLinkFrameworkException.class,
                () -> owner.deserialize(OtherProbe.class));
            assertEquals(ZLinkFrameworkErrorKind.TYPE_MISMATCH, mismatch.kind());
            assertEquals(1, serializer.calls.get());
        }
    }

    @Test
    void malformedFirstDecodeFailureIsSharedAcrossRepeatedAndConcurrentAccessors()
        throws Exception {
        ZLinkFrameworkException malformed = new ZLinkFrameworkException(
            ZLinkFrameworkErrorKind.PROTOCOL_ERROR, "malformed");
        CountingSerializer serializer = new CountingSerializer(null, malformed);
        try (Message payload = Message.from("payload")) {
            ZLinkInboundPayloadOwner owner = new ZLinkInboundPayloadOwner(payload, serializer);
            var executor = Executors.newFixedThreadPool(8);
            try {
                List<Future<Object>> results = java.util.stream.IntStream.range(0, 16)
                    .mapToObj(index -> executor.submit(() -> owner.deserialize(
                        index % 2 == 0 ? Probe.class : OtherProbe.class)))
                    .toList();
                for (Future<Object> result : results) {
                    ExecutionException failure = assertThrows(
                        ExecutionException.class,
                        () -> result.get(5, TimeUnit.SECONDS));
                    assertSame(malformed, failure.getCause());
                }
            } finally {
                executor.shutdownNow();
            }
            assertSame(
                malformed,
                assertThrows(
                    ZLinkFrameworkException.class,
                    () -> owner.deserialize(OtherProbe.class)));
            assertEquals(1, serializer.calls.get());
        }
    }

    @Test
    void productionSubscriptionFanoutBorrowsOneDecodedValue() {
        Probe decoded = new Probe("decoded");
        CountingSerializer serializer = new CountingSerializer(decoded, null);
        FirstHandler first = new FirstHandler();
        SecondHandler second = new SecondHandler();
        ZLinkSpotHandlerInvoker invoker = new ZLinkSpotHandlerInvoker(serializer, List.of());
        SpotSubscriptionHandlerRegistration firstRegistration = registration(FirstHandler.class);
        SpotSubscriptionHandlerRegistration secondRegistration = registration(SecondHandler.class);

        try (Message payload = Message.from("payload")) {
            ZLinkInboundPayloadOwner owner = invoker.payloadOwner(payload, null);
            Object value = invoker.deserializeSubscription(firstRegistration, owner);
            invoker.invokeSubscriptionDecoded(
                    firstRegistration, new Object(), "channel", "topic", Optional.empty(),
                    value, null, Map.of(), ignored -> first)
                .toCompletableFuture().join();
            invoker.invokeSubscriptionDecoded(
                    secondRegistration, new Object(), "channel", "topic", Optional.empty(),
                    value, null, Map.of(), ignored -> second)
                .toCompletableFuture().join();
        }

        assertSame(decoded, first.received);
        assertSame(decoded, second.received);
        assertEquals(1, serializer.calls.get());
    }

    private static SpotSubscriptionHandlerRegistration registration(Class<?> handlerType) {
        return new SpotSubscriptionHandlerRegistration(
            "topic", handlerType, null, Object.class, Probe.class, "Probe");
    }

    private static JsonNode fixture() throws Exception {
        return new ObjectMapper().readTree(Files.readString(sharedFixture()));
    }

    private static Path sharedFixture() {
        Path current = Path.of(System.getProperty("user.dir")).toAbsolutePath();
        while (current != null) {
            Path candidate = current.resolve(
                "runtime/conformance/payload-ownership-v1.json");
            if (Files.isRegularFile(candidate)) {
                return candidate;
            }
            current = current.getParent();
        }
        throw new IllegalStateException("shared payload ownership fixture was not found");
    }

    private record Probe(String value) {
    }

    private record OtherProbe(String value) {
    }

    private static final class FirstHandler
        implements ZLinkSpotSubscriptionHandler<Object, Probe> {
        private Probe received;

        @Override
        public CompletableFuture<Void> handle(Object spot, Probe event) {
            received = event;
            return CompletableFuture.completedFuture(null);
        }
    }

    private static final class SecondHandler
        implements ZLinkSpotSubscriptionHandler<Object, Probe> {
        private Probe received;

        @Override
        public CompletableFuture<Void> handle(Object spot, Probe event) {
            received = event;
            return CompletableFuture.completedFuture(null);
        }
    }

    private static final class CountingSerializer implements ZLinkMessageSerializer {
        private final Object decoded;
        private final RuntimeException failure;
        private final CountDownLatch entered;
        private final CountDownLatch release;
        private final AtomicInteger calls = new AtomicInteger();

        private CountingSerializer(Object decoded, RuntimeException failure) {
            this(decoded, failure, null, null);
        }

        private CountingSerializer(
            Object decoded,
            RuntimeException failure,
            CountDownLatch entered,
            CountDownLatch release) {
            this.decoded = decoded;
            this.failure = failure;
            this.entered = entered;
            this.release = release;
        }

        @Override
        public <T> ZLinkEncodedPayload serialize(T value) {
            throw new UnsupportedOperationException();
        }

        @Override
        public <T> T deserialize(ZLinkEncodedPayload payload, Class<T> type) {
            calls.incrementAndGet();
            if (entered != null) {
                entered.countDown();
            }
            if (release != null) {
                try {
                    if (!release.await(5, TimeUnit.SECONDS)) {
                        throw new AssertionError("decode release timed out");
                    }
                } catch (InterruptedException interrupted) {
                    Thread.currentThread().interrupt();
                    throw new AssertionError(interrupted);
                }
            }
            if (failure != null) {
                throw failure;
            }
            return type.cast(decoded);
        }
    }
}
