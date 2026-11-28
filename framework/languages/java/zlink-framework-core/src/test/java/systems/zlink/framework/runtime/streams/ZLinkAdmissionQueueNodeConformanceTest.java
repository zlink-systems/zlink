package systems.zlink.framework.runtime.streams;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertTrue;

import com.fasterxml.jackson.databind.JsonNode;

import org.junit.jupiter.params.ParameterizedTest;
import org.junit.jupiter.params.provider.Arguments;
import org.junit.jupiter.params.provider.MethodSource;

import systems.zlink.framework.runtime.streams.ZLinkAdmissionQueueMessages.AdmitPermit;
import systems.zlink.framework.runtime.streams.ZLinkAdmissionQueueMessages.AdmitPermitReply;
import systems.zlink.framework.runtime.streams.ZLinkAdmissionQueueMessages.Enqueue;
import systems.zlink.framework.runtime.streams.ZLinkAdmissionQueueMessages.EnqueueReply;
import systems.zlink.framework.runtime.streams.ZLinkAdmissionQueueMessages.NodeReport;
import systems.zlink.framework.runtime.streams.ZLinkAdmissionQueueMessages.NodeReportReply;
import systems.zlink.framework.runtime.streams.ZLinkAdmissionQueueMessages.ReleasePermit;

import java.nio.charset.StandardCharsets;
import java.util.List;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import java.util.concurrent.TimeoutException;
import java.util.concurrent.atomic.AtomicInteger;
import java.util.concurrent.atomic.AtomicLong;
import java.util.stream.Stream;

/** Runs the nodeScenarios of the shared login admission queue fixture against the node side. */
final class ZLinkAdmissionQueueNodeConformanceTest {
    private static final long NOW_MS = 1_700_000_000_000L;
    private static final long NODE_GENERATION = 7;
    private static final String STREAM = "login";
    private static final byte[] NODE_RID = "node-a".getBytes(StandardCharsets.UTF_8);
    private static final byte[] SESSION = "session-a".getBytes(StandardCharsets.UTF_8);

    static Stream<Arguments> nodeScenarios() throws Exception {
        return AdmissionQueueFixture.scenarios("nodeScenarios", 10);
    }

    @ParameterizedTest(name = "{0}")
    @MethodSource("nodeScenarios")
    void conformsToSharedNodeScenario(String name, JsonNode scenario) {
        new Run(scenario.required("given")).play(scenario.required("steps"));
    }

    private static final class FakeSpot implements ZLinkAdmissionQueueNode.SpotLink {
        final String permit;
        final long epoch;
        final AtomicInteger admitRequests = new AtomicInteger();
        final AtomicInteger releases = new AtomicInteger();
        long lastRequestSeq;

        FakeSpot(String permit, long epoch) {
            this.permit = permit;
            this.epoch = epoch;
        }

        @Override
        public CompletionStage<NodeReportReply> report(NodeReport report) {
            return CompletableFuture.completedFuture(new NodeReportReply(epoch, List.of()));
        }

        @Override
        public CompletionStage<EnqueueReply> enqueue(Enqueue enqueue) {
            throw new AssertionError("enqueue is not part of the node scenarios");
        }

        @Override
        public CompletionStage<AdmitPermitReply> admitPermit(AdmitPermit request) {
            admitRequests.incrementAndGet();
            lastRequestSeq = request.requestSeq();
            return switch (permit) {
                case "granted" ->
                        CompletableFuture.completedFuture(new AdmitPermitReply(true, 777L));
                case "denied" ->
                        CompletableFuture.completedFuture(new AdmitPermitReply(false, null));
                default ->
                        CompletableFuture.failedFuture(
                                new TimeoutException("Spot request timeout"));
            };
        }

        @Override
        public void releasePermit(ReleasePermit release) {
            releases.incrementAndGet();
        }
    }

    private static long epoch(JsonNode given) {
        return given.hasNonNull("reportedEpoch") ? given.get("reportedEpoch").asLong() : 42;
    }

    private static final class Run {
        private final AtomicLong clock = new AtomicLong(NOW_MS);
        private final ZLinkAdmissionTickets tickets =
                new ZLinkAdmissionTickets(AdmissionQueueFixture.KEY, null);
        private final ZLinkAdmissionQueueNode node;
        private final FakeSpot spot;
        private final JsonNode given;
        private final AtomicInteger stageOneInFlight = new AtomicInteger();
        private int handlerCalls;
        private long permitId;

        Run(JsonNode given) {
            this.given = given;
            boolean limited = given.hasNonNull("globalLimit");
            spot =
                    new FakeSpot(
                            AdmissionQueueFixture.text(given, "spotPermit", "granted"),
                            epoch(given));
            node =
                    new ZLinkAdmissionQueueNode(
                            tickets,
                            limited ? given.get("globalLimit").asInt() : null,
                            new ZLinkAdmissionQueueNode.Identity(NODE_RID, NODE_GENERATION),
                            () -> new ZLinkAdmissionQueueNode.OwnerLease("owner", 3),
                            () -> 5,
                            stageOneInFlight::get,
                            push -> {},
                            spot,
                            clock::get);
            if (given.hasNonNull("reportedEpoch")) {
                node.reportOnce().toCompletableFuture().join();
            }
        }

        void play(JsonNode steps) {
            for (JsonNode step : steps) {
                JsonNode expect = step.required("expect");
                switch (step.required("action").asText()) {
                    case "authenticate" -> authenticate(expect);
                    case "handlerCompleted" -> {
                        node.releasePermit(permitId);
                        assertEquals(
                                expect.required("releasePermitSends").asInt(), spot.releases.get());
                    }
                    case "nodeReport" -> {
                        NodeReport report = node.buildReport();
                        assertEquals(
                                expect.required("resolvedRequestSeq").asLong(),
                                report.resolvedRequestSeq().longValue());
                        assertTrue(report.heldPermits().isEmpty());
                    }
                    default ->
                            throw new AssertionError(
                                    "unknown action: " + step.required("action").asText());
                }
            }
        }

        /** Mirrors the ingress order: ticket, stage one, AdmitPermit, handler. */
        private void authenticate(JsonNode expect) {
            int stageOneBefore = stageOneInFlight.get();
            String decision;
            String errorCode = null;
            boolean queueRequired = false;
            if (!node.ticketAccepted(STREAM, SESSION, ticket())) {
                decision = "queueRequired";
                errorCode = "unavailable";
                queueRequired = true;
            } else {
                stageOneInFlight.incrementAndGet();
                assertEquals(0, handlerCalls);
                decision = "stageOne";
                var outcome = node.acquirePermit(STREAM, SESSION).toCompletableFuture().join();
                if (outcome instanceof ZLinkAdmissionQueueNode.Granted granted) {
                    permitId = granted.permitId();
                    node.ticketUsed(STREAM, SESSION);
                    handlerCalls++;
                } else {
                    stageOneInFlight.decrementAndGet();
                    errorCode = "unavailable";
                    queueRequired = outcome instanceof ZLinkAdmissionQueueNode.Denied;
                }
            }
            if (expect.has("decision")) {
                assertEquals(expect.get("decision").asText(), decision);
                assertEquals(expect.required("handlerCallsBeforeStageOne").asInt(), 0);
            }
            if (expect.has("errorCode")) {
                assertEquals(expect.get("errorCode").asText(), errorCode);
            }
            if (expect.has("metadata")) {
                assertTrue(queueRequired);
                assertEquals(
                        "1", expect.get("metadata").get("$zlink.admission.queueRequired").asText());
            }
            if (expect.has("handlerCalls")) {
                assertEquals(expect.get("handlerCalls").asInt(), handlerCalls);
            }
            if (expect.has("stageOneInFlightDelta")) {
                assertEquals(
                        expect.get("stageOneInFlightDelta").asInt(),
                        stageOneInFlight.get() - stageOneBefore);
            }
            if (expect.has("admitPermitRequests")) {
                assertEquals(expect.get("admitPermitRequests").asInt(), spot.admitRequests.get());
            }
            if (expect.has("admitPermitRequestSeq")) {
                assertEquals(expect.get("admitPermitRequestSeq").asLong(), spot.lastRequestSeq);
            }
            if (expect.has("releasePermitSends")) {
                assertEquals(expect.get("releasePermitSends").asInt(), spot.releases.get());
            }
        }

        private String ticket() {
            long epoch = epoch(given);
            var connection =
                    new ZLinkAdmissionTickets.TicketConnection(
                            NODE_RID, NODE_GENERATION, STREAM, SESSION);
            return switch (given.required("ticketState").asText()) {
                case "missing" -> null;
                case "bad-signature" ->
                        tickets.issue(
                                new ZLinkAdmissionTickets.TicketConnection(
                                        NODE_RID,
                                        NODE_GENERATION,
                                        STREAM,
                                        "session-b".getBytes(StandardCharsets.UTF_8)),
                                NOW_MS + 30_000,
                                epoch);
                case "expired" -> tickets.issue(connection, NOW_MS - 5_000, epoch);
                case "wrong-epoch" -> tickets.issue(connection, NOW_MS + 30_000, epoch - 1);
                case "valid" -> tickets.issue(connection, NOW_MS + 30_000, epoch);
                default -> throw new AssertionError("unknown ticketState");
            };
        }
    }
}
