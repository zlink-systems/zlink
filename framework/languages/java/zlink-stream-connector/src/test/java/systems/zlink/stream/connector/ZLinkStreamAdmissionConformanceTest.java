package systems.zlink.stream.connector;

import static org.junit.jupiter.api.Assertions.*;

import com.fasterxml.jackson.databind.JsonNode;
import com.fasterxml.jackson.databind.ObjectMapper;

import org.junit.jupiter.api.DynamicTest;
import org.junit.jupiter.api.Test;
import org.junit.jupiter.api.TestFactory;
import org.junit.jupiter.params.ParameterizedTest;
import org.junit.jupiter.params.provider.ValueSource;

import systems.zlink.contracts.messaging.Message;

import java.io.IOException;
import java.net.URI;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.Path;
import java.time.Duration;
import java.util.ArrayDeque;
import java.util.ArrayList;
import java.util.Base64;
import java.util.List;
import java.util.Map;
import java.util.Objects;
import java.util.PriorityQueue;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionException;
import java.util.concurrent.CompletionStage;
import java.util.concurrent.Delayed;
import java.util.concurrent.FutureTask;
import java.util.concurrent.ScheduledFuture;
import java.util.concurrent.ScheduledThreadPoolExecutor;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicInteger;
import java.util.stream.Stream;
import java.util.stream.StreamSupport;

final class ZLinkStreamAdmissionConformanceTest {
    private static final String RETRY_AFTER = "$zlink.admission.retryAfterMs";
    private static final ObjectMapper MAPPER = new ObjectMapper();

    @TestFactory
    Stream<DynamicTest> stageOneFixture() throws IOException {
        Path root = Path.of("").toAbsolutePath();
        Path fixture = null;
        while (root != null) {
            fixture = root.resolve("framework/runtime/conformance/login-admission-v1.json");
            if (Files.isRegularFile(fixture)) {
                break;
            }
            root = root.getParent();
        }
        assertNotNull(root, "login admission fixture not found");
        // Ticket and queue actions belong to stage two. This fixture's connectorScenarios
        // contain only stage-one Error and Response actions.
        return StreamSupport.stream(
                        MAPPER.readTree(fixture.toFile()).get("connectorScenarios").spliterator(),
                        false)
                .map(
                        scenario ->
                                DynamicTest.dynamicTest(
                                        scenario.get("name").asText(),
                                        () -> runScenario(scenario)));
    }

    private static void runScenario(JsonNode scenario) throws Exception {
        var given = scenario.get("given");
        var expect = scenario.get("expect");
        Duration timeout = Duration.ofMillis(given.get("requestTimeoutMs").asLong());
        Duration maxWait =
                given.get("maxAdmissionWaitMs").isNull()
                        ? null
                        : Duration.ofMillis(given.get("maxAdmissionWaitMs").asLong());
        var packetName = given.get("packetName").asText();
        var body = Base64.getDecoder().decode(given.get("payloadBase64").asText());
        var metadata = new java.util.HashMap<String, String>();
        given.get("applicationMetadata")
                .fields()
                .forEachRemaining(field -> metadata.put(field.getKey(), field.getValue().asText()));
        try (var client = new Client(timeout, maxWait)) {
            var result = client.request(packetName, body, metadata);
            client.sendingContext.setMetadata("hook", "changed after submission");
            AtomicInteger callbacks = new AtomicInteger();
            result.whenComplete((reply, failure) -> callbacks.incrementAndGet());

            for (var response : given.get("responses")) {
                client.clock.advanceTo(response.get("atElapsedMs").asLong());
                long requestSeq = Long.parseLong(response.get("requestSeq").asText());
                if (response.get("kind").asText().equals("Error")) {
                    client.jitters.add(response.get("jitterMs").asInt());
                    String hint =
                            response.get("retryAfterMs").isNull()
                                    ? null
                                    : response.get("retryAfterMs").asText();
                    client.transport.error(requestSeq, response.get("remoteCode").asText(), hint);
                } else {
                    client.transport.respond(requestSeq);
                }
            }
            if (expect.has("terminalAtElapsedMs")) {
                long deadline = expect.get("terminalAtElapsedMs").asLong();
                client.clock.advanceTo(deadline - 1);
                assertFalse(result.isDone(), "request completed before its deadline");
                client.clock.advanceTo(deadline);
            }
            String expectedResult = expect.get("result").asText();
            if (expectedResult.equals("success")) {
                assertTrue(result.isDone(), "request still pending");
                result.join().payload().close();
            } else {
                assertError(
                        result,
                        ZLinkStreamErrorCode.valueOf(
                                expectedResult
                                        .replaceAll("([a-z])([A-Z])", "$1_$2")
                                        .toUpperCase(java.util.Locale.ROOT)));
            }
            assertEquals(expect.get("waitingCallbacks").asInt(), client.waiting.size());
            assertEquals(expect.get("requestSendingHooks").asInt(), client.sending.get());
            assertEquals(expect.get("replyReceivedHooks").asInt(), client.replies.get());
            assertEquals(expect.get("requestCallbacks").asInt(), callbacks.get());
            assertTrue(client.connector.isConnected());

            List<Sent> sent = client.transport.sent;
            assertEquals(expect.get("sendAtElapsedMs").size(), sent.size());
            for (int i = 0; i < sent.size(); i++) {
                var frame = sent.get(i);
                assertEquals(expect.get("sendAtElapsedMs").get(i).asLong(), frame.elapsedMs());
                assertEquals(
                        expect.get("requestSeqs").get(i).asText(),
                        Long.toUnsignedString(frame.header().requestSeq()));
                assertEquals(packetName, frame.header().name());
                assertArrayEquals(body, frame.body());
                var expectedMetadata = new java.util.HashMap<>(metadata);
                expectedMetadata.put("hook", "once");
                assertEquals(expectedMetadata, frame.header().metadata());
                // The public request API assigns its own correlation ID.
                assertFalse(frame.header().correlationId().isBlank());
                assertEquals(
                        sent.getFirst().header().correlationId(), frame.header().correlationId());
            }
        }
    }

    @ParameterizedTest
    @ValueSource(booleans = {false, true})
    void cancellationOrDisconnectEndsWaitingRequest(boolean disconnect) throws Exception {
        try (var client = new Client(Duration.ofSeconds(3), null)) {
            var result = client.request("Login", new byte[0], Map.of());
            client.transport.error(1, "unavailable", "100");
            assertEquals(List.of(Duration.ofMillis(100)), client.waiting);
            var replacement = new Transport(client.clock);
            if (disconnect) {
                client.transport.disconnect();
                assertError(result, ZLinkStreamErrorCode.DISCONNECTED);
                client.connector.lifecycle.activateConnection(replacement);
            } else {
                assertTrue(result.cancel(false));
            }
            client.clock.advanceTo(3000);
            assertEquals(1, client.transport.sent.size());
            assertEquals(disconnect ? 1 : 0, client.replies.get());
            assertTrue(replacement.sent.isEmpty());
        }
    }

    @Test
    void queuedRetryDoesNotWriteAfterReadFailureClaimsTheConnection() {
        try (var client = new Client(Duration.ofSeconds(3), null);
                var body = Message.from(new byte[0])) {
            var result = client.request("Login", new byte[0], Map.of());
            client.transport.error(1, "unavailable", "100");

            var activeWrite = new CompletableFuture<Void>();
            client.transport.nextWrite = activeWrite;
            var preceding =
                    client.connector
                            .send(new ZLinkStreamEncodedPayload("Other", body, Map.of()))
                            .submit()
                            .toCompletableFuture();
            client.clock.advanceTo(100);
            assertEquals(2, client.transport.sent.size());
            assertFalse(preceding.isDone());

            client.connector.onErrorReceived(
                    error -> {
                        activeWrite.complete(null);
                        return CompletableFuture.completedFuture(null);
                    });
            client.transport.disconnect();

            assertError(result, ZLinkStreamErrorCode.DISCONNECTED);
            assertEquals(2, client.transport.sent.size());
        }
    }

    @Test
    void immediateWaitingCallbackCanCancelADueNowRetry() {
        try (var client = new Client(Duration.ofSeconds(3), null)) {
            var result = client.request("Login", new byte[0], Map.of());
            client.connector.onAdmissionWaiting(
                    event -> {
                        result.cancel(false);
                        return CompletableFuture.completedFuture(null);
                    });
            client.transport.error(1, "unavailable", "0");
            client.clock.advanceTo(3000);
            assertTrue(result.isCancelled());
            assertEquals(1, client.transport.sent.size());
            assertEquals(0, client.replies.get());
        }
    }

    @Test
    void retryUsesTheOriginalActorHandle() {
        try (var client = new Client(Duration.ofSeconds(3), null)) {
            client.transport.control(ZLinkStreamActorRegistry.BOUND, new byte[] {1, 0, 7, 1, 'a'});
            var actor = client.connector.actor("a").orElseThrow();
            try (var body = Message.from(new byte[0])) {
                var result =
                        actor.request(new ZLinkStreamEncodedPayload("Login", body, Map.of()))
                                .submit()
                                .toCompletableFuture();
                assertEquals(7, client.transport.sent.getFirst().header().actorSlot());
                client.transport.error(1, "unavailable", "100");
                client.transport.control(ZLinkStreamActorRegistry.UNBOUND, new byte[] {1, 0, 7});
                client.transport.control(
                        ZLinkStreamActorRegistry.BOUND, new byte[] {1, 0, 7, 1, 'b'});
                client.clock.advanceTo(100);
                assertError(result, ZLinkStreamErrorCode.VALIDATION_FAILED);
                assertEquals(1, client.transport.sent.size());
            }
        }
    }

    @Test
    void maximumRetryHintIsAcceptedWithoutOverflow() throws Exception {
        long retry = Integer.MAX_VALUE;
        try (var client = new Client(Duration.ofMillis(retry + 100), null)) {
            var result = client.request("Login", new byte[0], Map.of());
            client.jitters.add(50);
            client.transport.error(1, "unavailable", Long.toString(retry));
            assertEquals(List.of(Duration.ofMillis(retry)), client.waiting);
            client.clock.advanceTo(retry + 49);
            assertEquals(1, client.transport.sent.size());
            client.clock.advanceTo(retry + 50);
            assertEquals(2, client.transport.sent.size());
            client.transport.respond(2);
            result.join().payload().close();
        }
    }

    @ParameterizedTest
    @ValueSource(longs = {0, -1})
    void nonpositiveAdmissionWaitFailsOptionValidation(long millis) {
        var exception =
                assertThrows(
                        ZLinkStreamException.class,
                        () ->
                                ZLinkStreamConnectorFactory.create(
                                        options(Duration.ofSeconds(3), Duration.ofMillis(millis))));
        assertEquals(ZLinkStreamErrorCode.VALIDATION_FAILED, exception.errorCode());
    }

    @Test
    void hintOnAnotherRemoteErrorDoesNotTriggerAdmissionRetry() {
        try (var client = new Client(Duration.ofSeconds(3), null)) {
            var result = client.request("Login", new byte[0], Map.of());
            client.transport.error(1, "internal", "100");
            assertError(result, ZLinkStreamErrorCode.REMOTE_ERROR);
            client.clock.advanceTo(3000);
            assertTrue(client.waiting.isEmpty());
            assertEquals(1, client.transport.sent.size());
        }
    }

    @Test
    void malformedHintFailsOnlyItsRequest() {
        try (var client = new Client(Duration.ofSeconds(3), null)) {
            var result = client.request("Login", new byte[0], Map.of());
            var other = client.request("Other", new byte[0], Map.of());
            client.transport.error(1, "unavailable", "bad");
            assertError(result, ZLinkStreamErrorCode.FRAME_DECODE_FAILED);
            client.transport.respond(2);
            other.join().payload().close();
            assertTrue(client.connector.isConnected());
        }
    }

    private static ZLinkStreamConnectorOptions options(Duration timeout, Duration maxWait) {
        return new ZLinkStreamConnectorOptions(
                URI.create("tcp://127.0.0.1:1"),
                ZLinkStreamDispatchMode.IMMEDIATE,
                timeout,
                Duration.ofSeconds(1),
                1,
                Duration.ofSeconds(1),
                65536,
                65536,
                false,
                Duration.ofSeconds(1),
                Duration.ofSeconds(1),
                false,
                Duration.ofSeconds(1),
                Duration.ofSeconds(1),
                1.0,
                false,
                ZLinkStreamCompression.NONE,
                null,
                null,
                null,
                maxWait);
    }

    private static void assertError(
            CompletableFuture<ZLinkStreamEncodedPayload> result, ZLinkStreamErrorCode expected) {
        assertTrue(result.isDone(), "request still pending");
        assertEquals(
                expected,
                assertInstanceOf(
                                ZLinkStreamException.class,
                                assertThrows(CompletionException.class, result::join).getCause())
                        .errorCode());
    }

    private static final class Client implements AutoCloseable {
        final Clock clock = new Clock();
        final Transport transport = new Transport(clock);
        final ArrayDeque<Integer> jitters = new ArrayDeque<>();
        final List<Duration> waiting = new ArrayList<>();
        final AtomicInteger sending = new AtomicInteger();
        final AtomicInteger replies = new AtomicInteger();
        final DefaultZLinkStreamConnector connector;
        ZLinkStreamRequestSendingContext sendingContext;

        Client(Duration timeout, Duration maxWait) {
            var options = options(timeout, maxWait);
            connector =
                    new DefaultZLinkStreamConnector(
                            options,
                            clock,
                            () -> clock.nanos,
                            () -> jitters.isEmpty() ? 0 : jitters.remove());
            connector.onRequestSending(
                    context -> {
                        sending.incrementAndGet();
                        sendingContext = context;
                        context.setMetadata("hook", "once");
                    });
            connector.onReplyReceived(
                    context -> {
                        replies.incrementAndGet();
                        if (context.reply() != null) {
                            context.reply().payload().payload().close();
                        }
                    });
            connector.onAdmissionWaiting(
                    event -> {
                        waiting.add(event.retryAfter());
                        return CompletableFuture.completedFuture(null);
                    });
            connector.lifecycle.activateConnection(transport);
        }

        CompletableFuture<ZLinkStreamEncodedPayload> request(
                String name, byte[] body, Map<String, String> metadata) {
            try (Message message = Message.from(body)) {
                return connector
                        .request(new ZLinkStreamEncodedPayload(name, message, metadata))
                        .submit()
                        .toCompletableFuture();
            }
        }

        @Override
        public void close() {
            connector.close().submit().toCompletableFuture().join();
            clock.shutdownNow();
        }
    }

    private record Sent(long elapsedMs, ZLinkStreamWireProtocol.Header header, byte[] body) {}

    private static final class Transport implements ZLinkStreamTransportConnection {
        final Clock clock;
        final List<Sent> sent = new ArrayList<>();
        CompletableFuture<ZLinkStreamWireProtocol.Frame> receive;
        CompletableFuture<Void> nextWrite;
        boolean open = true;

        Transport(Clock clock) {
            this.clock = clock;
        }

        public CompletionStage<ZLinkStreamWireProtocol.Frame> readFrameAsync() {
            receive = new CompletableFuture<>();
            return receive;
        }

        public CompletionStage<Void> writeAsync(byte[] bytes) {
            var frame = ZLinkStreamWireProtocol.decodeFrame(bytes);
            sent.add(
                    new Sent(
                            TimeUnit.NANOSECONDS.toMillis(clock.nanos),
                            ZLinkStreamWireProtocol.decodeHeader(frame.header()),
                            frame.payload()));
            if (nextWrite != null) {
                var write = nextWrite;
                nextWrite = null;
                return write;
            }
            return CompletableFuture.completedFuture(null);
        }

        public boolean isOpen() {
            return open;
        }

        public void close() {
            open = false;
        }

        void error(long seq, String code, String retryAfter) {
            deliver(
                    ZLinkStreamWireProtocol.KIND_ERROR,
                    ZLinkStreamWireProtocol.CODEC_JSON,
                    seq,
                    retryAfter == null ? Map.of() : Map.of(RETRY_AFTER, retryAfter),
                    ("{\"code\":\""
                                    + code
                                    + "\",\"message\":\"Authentication admission is full.\"}")
                            .getBytes(StandardCharsets.UTF_8));
        }

        void respond(long seq) {
            deliver(
                    ZLinkStreamWireProtocol.KIND_RESPONSE,
                    ZLinkStreamWireProtocol.CODEC_RAW,
                    seq,
                    Map.of(),
                    new byte[0]);
        }

        void deliver(int kind, int codec, long seq, Map<String, String> metadata, byte[] body) {
            var header =
                    new ZLinkStreamWireProtocol.Header(
                            kind,
                            codec,
                            ZLinkStreamWireProtocol.FLAG_HAS_REQUEST_SEQ,
                            seq,
                            "",
                            metadata,
                            null);
            assertTrue(
                    receive.complete(
                            new ZLinkStreamWireProtocol.Frame(
                                    ZLinkStreamWireProtocol.encodeHeader(header), body)));
        }

        void control(String name, byte[] body) {
            var header =
                    new ZLinkStreamWireProtocol.Header(
                            ZLinkStreamWireProtocol.KIND_CONTROL,
                            ZLinkStreamWireProtocol.CODEC_RAW,
                            0,
                            null,
                            name,
                            Map.of(),
                            null);
            assertTrue(
                    receive.complete(
                            new ZLinkStreamWireProtocol.Frame(
                                    ZLinkStreamWireProtocol.encodeHeader(header), body)));
        }

        void disconnect() {
            receive.completeExceptionally(new IOException("test peer closed"));
        }
    }

    private static final class Clock extends ScheduledThreadPoolExecutor {
        final PriorityQueue<Task> tasks = new PriorityQueue<>();
        long nanos;
        long order;

        Clock() {
            super(1);
        }

        @Override
        public ScheduledFuture<?> schedule(Runnable command, long delay, TimeUnit unit) {
            var task = new Task(command, nanos + Math.max(0, unit.toNanos(delay)), order++);
            tasks.add(task);
            return task;
        }

        void advanceTo(long millis) {
            long target = TimeUnit.MILLISECONDS.toNanos(millis);
            assertTrue(target >= nanos);
            while (!tasks.isEmpty() && tasks.peek().deadline <= target) {
                var next = tasks.remove();
                nanos = next.deadline;
                next.run();
            }
            nanos = target;
        }

        private final class Task extends FutureTask<Void> implements ScheduledFuture<Void> {
            final long deadline;
            final long order;

            Task(Runnable command, long deadline, long order) {
                super(Objects.requireNonNull(command), null);
                this.deadline = deadline;
                this.order = order;
            }

            public long getDelay(TimeUnit unit) {
                return unit.convert(deadline - nanos, TimeUnit.NANOSECONDS);
            }

            public int compareTo(Delayed other) {
                Task task = (Task) other;
                int byDeadline = Long.compare(deadline, task.deadline);
                return byDeadline == 0 ? Long.compare(order, task.order) : byDeadline;
            }
        }
    }
}
