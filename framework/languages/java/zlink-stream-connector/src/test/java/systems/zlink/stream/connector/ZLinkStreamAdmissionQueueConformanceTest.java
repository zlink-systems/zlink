package systems.zlink.stream.connector;

import static org.junit.jupiter.api.Assertions.*;

import static systems.zlink.stream.connector.ZLinkStreamWireProtocol.*;

import com.fasterxml.jackson.databind.JsonNode;
import com.fasterxml.jackson.databind.ObjectMapper;

import org.junit.jupiter.api.DynamicTest;
import org.junit.jupiter.api.TestFactory;

import systems.zlink.contracts.messaging.Message;

import java.net.URI;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.Path;
import java.time.Duration;
import java.util.ArrayList;
import java.util.HashMap;
import java.util.List;
import java.util.Map;
import java.util.PriorityQueue;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionException;
import java.util.concurrent.CompletionStage;
import java.util.concurrent.Delayed;
import java.util.concurrent.FutureTask;
import java.util.concurrent.ScheduledFuture;
import java.util.concurrent.ScheduledThreadPoolExecutor;
import java.util.concurrent.TimeUnit;
import java.util.stream.Stream;
import java.util.stream.StreamSupport;

final class ZLinkStreamAdmissionQueueConformanceTest {
    private static final ObjectMapper JSON = new ObjectMapper();
    private static final String ENQUEUE = "$zlink.admission.enqueue";
    private static final String QUEUE_REQUIRED = "$zlink.admission.queueRequired";
    private static final String RETRY_AFTER = "$zlink.admission.retryAfterMs";
    private static final String POSITION = "$zlink.admission.position";
    private static final String TICKET = "$zlink.admission.ticket";
    private static final String WIRE_TICKET =
            "AQAAAYvP5d0wAAAAAAAAACpkd-u3yih1dIog4S6zkov_tIe5kI5BZpnfdXmWl3x7kQ";

    private static final String OLD_TICKET =
            "AQAAAYvP5d0wAAAAAAAAACoWJ0doAwUaJ4UVFk6fvB1oRc3kjk7SUHTTW8ThsOXQnA";

    @TestFactory
    Stream<DynamicTest> queueFixture() throws Exception {
        Path root = Path.of("").toAbsolutePath();
        while (root != null
                && !Files.isRegularFile(
                        root.resolve(
                                "framework/runtime/conformance/login-admission-queue-v1.json"))) {
            root = root.getParent();
        }
        assertNotNull(root, "login admission queue fixture not found");
        var fixture =
                JSON.readTree(
                        root.resolve("framework/runtime/conformance/login-admission-queue-v1.json")
                                .toFile());
        assertEquals(11, fixture.required("connectorScenarios").size());
        return StreamSupport.stream(fixture.required("connectorScenarios").spliterator(), false)
                .map(
                        scenario ->
                                DynamicTest.dynamicTest(
                                        scenario.required("name").asText(), () -> run(scenario)));
    }

    private static void run(JsonNode scenario) throws Exception {
        JsonNode given = scenario.required("given");
        var peer = new Peer(given);
        try {
            if (given.has("requests")) {
                for (var name : given.required("requests")) {
                    peer.request(name.asText());
                }
            } else {
                peer.request("a");
                peer.clock.advance(given.required("firstRejectionMs").asLong());
                peer.error("a", "unavailable", Map.of(QUEUE_REQUIRED, "1"));
            }
            if (given.has("cachedTicket")) {
                assertEquals("old", given.required("cachedTicket").asText());
                peer.primeTicket(given.required("requests").get(0).asText());
            }
            for (var step : scenario.required("steps")) {
                peer.step(step);
            }
        } finally {
            peer.connector.close().submit().toCompletableFuture().join();
            peer.clock.shutdownNow();
        }
    }

    private static final class Peer implements ZLinkStreamTransportConnection {
        final Clock clock = new Clock();
        final List<Header> sent = new ArrayList<>();
        final Map<String, CompletableFuture<ZLinkStreamEncodedPayload>> requests = new HashMap<>();
        final List<String> callbacks = new ArrayList<>();
        final List<ZLinkStreamError> errors = new ArrayList<>();
        final List<Map<String, Integer>> positions = new ArrayList<>();
        int queued;
        final DefaultZLinkStreamConnector connector;
        CompletableFuture<Frame> inbound;
        boolean open = true;

        Peer(JsonNode given) {
            Duration timeout = Duration.ofMillis(given.path("originalDeadlineMs").asLong(25000));
            Duration maxWait = Duration.ofMillis(given.path("maxAdmissionWaitMs").asLong(25000));
            Duration second = Duration.ofSeconds(1);
            connector =
                    new DefaultZLinkStreamConnector(
                            new ZLinkStreamConnectorOptions(
                                    URI.create("tcp://127.0.0.1:1"),
                                    ZLinkStreamDispatchMode.IMMEDIATE,
                                    timeout,
                                    second,
                                    1,
                                    second,
                                    65536,
                                    65536,
                                    false,
                                    second,
                                    second,
                                    false,
                                    second,
                                    second,
                                    1.0,
                                    false,
                                    ZLinkStreamCompression.NONE,
                                    null,
                                    null,
                                    null,
                                    maxWait),
                            clock,
                            () -> clock.nanos,
                            () -> 0);
            connector.onAdmissionWaiting(
                    event -> {
                        callbacks.add("OnAdmissionWaiting");
                        return CompletableFuture.completedFuture(null);
                    });
            connector.onAdmissionQueued(
                    () -> {
                        callbacks.add("OnAdmissionQueued");
                        queued++;
                        return CompletableFuture.completedFuture(null);
                    });
            connector.onAdmissionPosition(
                    (position, wait) -> {
                        callbacks.add("OnAdmissionPosition");
                        positions.add(Map.of("position", position, "estimatedWaitMs", wait));
                        return CompletableFuture.completedFuture(null);
                    });
            connector.onErrorReceived(
                    error -> {
                        errors.add(error);
                        return CompletableFuture.completedFuture(null);
                    });
            connector.lifecycle.activateConnection(this);
        }

        void request(String name) {
            try (Message body = Message.from(new byte[] {1})) {
                var result =
                        connector
                                .request(
                                        new ZLinkStreamEncodedPayload(
                                                name, body, Map.of("app", name)))
                                .submit()
                                .toCompletableFuture();
                requests.put(name, result);
                result.thenAccept(reply -> callbacks.add("requestComplete"));
            }
        }

        void primeTicket(String name) throws Exception {
            error(name, "unavailable", Map.of(QUEUE_REQUIRED, "1"));
            deliver(
                    KIND_RESPONSE,
                    last(ENQUEUE),
                    ENQUEUE,
                    Map.of(),
                    Map.of("ticket", OLD_TICKET, "expiresInMs", 30000));
            clock.advance(0);
            var replay = sent.getLast();
            assertEquals(OLD_TICKET, replay.metadata().get(TICKET));
            sent.removeIf(
                    header ->
                            header.name().equals(ENQUEUE)
                                    || (header.name().equals(name) && header != replay));
            queued = 0;
        }

        void step(JsonNode step) throws Exception {
            callbacks.clear();
            positions.clear();
            JsonNode expect = step.required("expect");
            if (expect.has("deadlineMs")) {
                clock.advance(expect.required("deadlineMs").asLong() - 1);
                assertFalse(requests.get("a").isDone());
            }
            if (step.has("atMs")) {
                clock.advance(step.required("atMs").asLong());
            }
            String name = step.path("request").asText("a");
            switch (step.required("action").asText()) {
                case "queueRequired" -> error(name, "unavailable", Map.of(QUEUE_REQUIRED, "1"));
                case "stageOneReject" -> {
                    error(
                            name,
                            "unavailable",
                            Map.of(RETRY_AFTER, step.required("retryAfterMs").asText()));
                    clock.advance(
                            TimeUnit.NANOSECONDS.toMillis(clock.nanos)
                                    + step.required("retryAfterMs").asLong());
                }
                case "positionResponse" ->
                        deliver(KIND_RESPONSE, last(ENQUEUE), ENQUEUE, Map.of(), position(step));
                case "positionSend" -> deliver(KIND_SEND, null, POSITION, Map.of(), position(step));
                case "ticketSend" -> {
                    assertEquals("T", step.required("ticket").asText());
                    deliver(
                            KIND_SEND,
                            null,
                            TICKET,
                            Map.of(),
                            Map.of(
                                    "ticket",
                                    WIRE_TICKET,
                                    "expiresInMs",
                                    step.required("expiresInMs").asInt()));
                    clock.advance(TimeUnit.NANOSECONDS.toMillis(clock.nanos));
                }
                case "complete" -> {
                    deliver(KIND_RESPONSE, last(name), name, Map.of(), Map.of());
                    requests.get(name).join().payload().close();
                    assertEquals(
                            WIRE_TICKET,
                            sent.stream()
                                    .filter(header -> header.name().equals(name))
                                    .reduce((first, next) -> next)
                                    .orElseThrow()
                                    .metadata()
                                    .get(TICKET));
                }
                case "error" -> {
                    Map<String, String> metadata = new HashMap<>();
                    step.required("metadata")
                            .properties()
                            .forEach(
                                    field ->
                                            metadata.put(
                                                    field.getKey(), field.getValue().asText()));
                    error(name, step.required("code").asText(), metadata);
                }
                case "enqueueResponse" -> {
                    // Both requests are pending. The first rejection owns the enqueue response.
                    error(name, "unavailable", Map.of(QUEUE_REQUIRED, "1"));
                    if (requests.containsKey("b")) {
                        error("b", "unavailable", Map.of(QUEUE_REQUIRED, "1"));
                    }
                    deliver(
                            KIND_RESPONSE,
                            last(ENQUEUE),
                            ENQUEUE,
                            Map.of(),
                            step.required("payload"));
                }
                case "send" -> {
                    for (String waiting : requests.keySet()) {
                        error(waiting, "unavailable", Map.of(QUEUE_REQUIRED, "1"));
                    }
                    deliver(
                            KIND_SEND,
                            null,
                            step.required("packet").asText(),
                            Map.of(),
                            step.required("payload"));
                }
                case "silence" -> {}
                case "wait" -> assertFailure("a", ZLinkStreamErrorCode.REQUEST_TIMEOUT);
                default -> fail("unknown action " + step);
            }
            if (expect.has("cachedTicket")) {
                assertTrue(expect.required("cachedTicket").isNull());
                assertEquals(1, sent.stream().filter(header -> header.name().equals(name)).count());
                assertNull(sent.getLast().metadata().get(TICKET));
            }
            if (expect.has("enqueueCount")) {
                assertEquals(
                        expect.required("enqueueCount").asLong(),
                        sent.stream().filter(header -> header.name().equals(ENQUEUE)).count());
            }
            if (expect.has("callbacks")) {
                assertEquals(
                        StreamSupport.stream(expect.required("callbacks").spliterator(), false)
                                .map(JsonNode::asText)
                                .toList(),
                        callbacks);
            }
            if (expect.has("queuedCallbacks")) {
                assertEquals(expect.required("queuedCallbacks").asInt(), queued);
            }
            if (callbacks.contains("OnAdmissionPosition")) {
                assertEquals(List.of(position(step)), positions);
            }
            if (expect.has("positionCallbacks")) {
                assertEquals(
                        expect.required("positionCallbacks").asLong(),
                        callbacks.stream().filter("OnAdmissionPosition"::equals).count());
            }
            if (expect.has("retriedRequests")) {
                for (var retry : expect.required("retriedRequests")) {
                    var attempts =
                            sent.stream()
                                    .filter(header -> header.name().equals(retry.asText()))
                                    .toList();
                    assertEquals(2, attempts.size());
                    assertEquals(WIRE_TICKET, attempts.getLast().metadata().get(TICKET));
                    assertEquals(
                            attempts.getFirst().correlationId(),
                            attempts.getLast().correlationId());
                    assertEquals(retry.asText(), attempts.getLast().metadata().get("app"));
                    assertNotEquals(
                            attempts.getFirst().requestSeq(), attempts.getLast().requestSeq());
                }
            }
            if (expect.path("discarded").asBoolean()) {
                assertEquals(0, connector.pendingDispatchCount());
                assertTrue(errors.isEmpty());
                assertTrue(callbacks.isEmpty());
            }
            if (expect.has("connectionClosed")) {
                assertEquals(expect.required("connectionClosed").asBoolean(), !open);
            }
            for (String request : requests.keySet()) {
                switch (expect.path(request).asText()) {
                    case "FrameDecodeFailed" ->
                            assertFailure(request, ZLinkStreamErrorCode.FRAME_DECODE_FAILED);
                    case "pending" -> assertFalse(requests.get(request).isDone());
                    default -> {}
                }
            }
            if (expect.has("sendAfterDeadline")) {
                int count = sent.size();
                clock.advance(expect.required("deadlineMs").asLong() + 15000);
                assertEquals(count, sent.size());
                assertEquals(
                        1, sent.stream().filter(header -> header.name().equals(ENQUEUE)).count());
            }
        }

        private static Map<String, Integer> position(JsonNode step) {
            return Map.of(
                    "position",
                    step.required("position").asInt(),
                    "estimatedWaitMs",
                    step.required("estimatedWaitMs").asInt());
        }

        private void assertFailure(String name, ZLinkStreamErrorCode code) {
            var request = requests.get(name);
            assertTrue(request.isDone(), "request still pending: " + name);
            assertEquals(
                    code,
                    assertInstanceOf(
                                    ZLinkStreamException.class,
                                    assertThrows(CompletionException.class, request::join)
                                            .getCause())
                            .errorCode());
        }

        private long last(String name) {
            return sent.stream()
                    .filter(header -> header.name().equals(name))
                    .reduce((first, next) -> next)
                    .orElseThrow(() -> new AssertionError("missing outbound " + name))
                    .requestSeq();
        }

        private void error(String name, String code, Map<String, String> metadata)
                throws Exception {
            deliver(
                    KIND_ERROR,
                    last(name),
                    name,
                    metadata,
                    Map.of("code", code, "message", "Error."));
        }

        private void deliver(
                int kind, Long seq, String name, Map<String, String> metadata, Object payload)
                throws Exception {
            var header =
                    new Header(
                            kind,
                            CODEC_JSON,
                            seq == null ? 0 : FLAG_HAS_REQUEST_SEQ,
                            seq,
                            name,
                            metadata,
                            null);
            assertTrue(
                    inbound.complete(
                            new Frame(encodeHeader(header), JSON.writeValueAsBytes(payload))));
        }

        @Override
        public CompletionStage<Frame> readFrameAsync() {
            inbound = new CompletableFuture<>();
            return inbound;
        }

        @Override
        public CompletionStage<Void> writeAsync(byte[] bytes) {
            var frame = decodeFrame(bytes);
            var header = decodeHeader(frame.header());
            if (header.name().equals(ENQUEUE)) {
                assertEquals("{}", new String(frame.payload(), StandardCharsets.UTF_8));
            } else {
                assertArrayEquals(new byte[] {1}, frame.payload());
            }
            sent.add(header);
            return CompletableFuture.completedFuture(null);
        }

        @Override
        public boolean isOpen() {
            return open;
        }

        @Override
        public void close() {
            open = false;
        }
    }

    private static final class Clock extends ScheduledThreadPoolExecutor {
        private final PriorityQueue<Task> tasks = new PriorityQueue<>();
        private long nanos;
        private long order;

        Clock() {
            super(1);
        }

        @Override
        public ScheduledFuture<?> schedule(Runnable task, long delay, TimeUnit unit) {
            var scheduled = new Task(task, nanos + unit.toNanos(delay), order++);
            tasks.add(scheduled);
            return scheduled;
        }

        void advance(long millis) {
            long target = TimeUnit.MILLISECONDS.toNanos(millis);
            assertTrue(target >= nanos);
            while (!tasks.isEmpty() && tasks.peek().deadline <= target) {
                var task = tasks.remove();
                nanos = task.deadline;
                task.run();
            }
            nanos = target;
        }

        private final class Task extends FutureTask<Void> implements ScheduledFuture<Void> {
            private final long deadline;
            private final long order;

            Task(Runnable task, long deadline, long order) {
                super(task, null);
                this.deadline = deadline;
                this.order = order;
            }

            @Override
            public long getDelay(TimeUnit unit) {
                return unit.convert(deadline - nanos, TimeUnit.NANOSECONDS);
            }

            @Override
            public int compareTo(Delayed other) {
                var task = (Task) other;
                int time = Long.compare(deadline, task.deadline);
                return time == 0 ? Long.compare(order, task.order) : time;
            }
        }
    }
}
