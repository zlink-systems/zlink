package systems.zlink.framework.runtime.streams;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertFalse;
import static org.junit.jupiter.api.Assertions.assertNotEquals;
import static org.junit.jupiter.api.Assertions.assertNotNull;
import static org.junit.jupiter.api.Assertions.assertTrue;

import com.fasterxml.jackson.databind.JsonNode;

import org.junit.jupiter.params.ParameterizedTest;
import org.junit.jupiter.params.provider.Arguments;
import org.junit.jupiter.params.provider.MethodSource;

import systems.zlink.framework.runtime.streams.ZLinkAdmissionQueueMessages.AdmitPermit;
import systems.zlink.framework.runtime.streams.ZLinkAdmissionQueueMessages.Connection;
import systems.zlink.framework.runtime.streams.ZLinkAdmissionQueueMessages.ConnectionRef;
import systems.zlink.framework.runtime.streams.ZLinkAdmissionQueueMessages.NodeReport;

import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.Base64;
import java.util.HashMap;
import java.util.List;
import java.util.Map;
import java.util.concurrent.atomic.AtomicLong;
import java.util.stream.Stream;

/** Runs the queueScenarios of the shared login admission queue fixture against the Spot state. */
final class ZLinkAdmissionQueueConformanceTest {
    private static final long START_MS = 1_700_000_000_000L;
    private static final String STREAM = "login";

    static Stream<Arguments> queueScenarios() throws Exception {
        return AdmissionQueueFixture.scenarios("queueScenarios", 20);
    }

    @ParameterizedTest(name = "{0}")
    @MethodSource("queueScenarios")
    void conformsToSharedQueueScenario(String name, JsonNode scenario) {
        new Run(scenario.required("given")).play(scenario.required("steps"));
    }

    private static final class Run {
        private final AtomicLong clock = new AtomicLong(START_MS);
        private final ZLinkAdmissionTickets tickets =
                new ZLinkAdmissionTickets(AdmissionQueueFixture.KEY, null);
        private final Integer globalLimit;
        private final JsonNode given;
        private final Map<String, Long> permitAliases = new HashMap<>();
        private final Map<String, String> ticketAliases = new HashMap<>();
        private long epoch;
        private ZLinkAdmissionQueueState state;
        private long nextSeededPermit = 1000;
        private long lastPermit;
        private final java.util.Set<String> invalidated = new java.util.HashSet<>();

        Run(JsonNode given) {
            this.given = given;
            epoch = AdmissionQueueFixture.number(given, "epoch", 42);
            globalLimit = given.has("globalLimit") ? given.get("globalLimit").asInt() : null;
            state = new ZLinkAdmissionQueueState(tickets, globalLimit, clock::get, epoch);
            if (given.has("position")) {
                for (long ahead = 1; ahead < given.get("position").asLong(); ahead++) {
                    state.enqueue(waiter("other", 1, "ahead" + ahead));
                }
                state.enqueue(
                        waiter(
                                given.required("nodeRid").asText(),
                                1,
                                given.required("sessionRid").asText()));
            }
            for (String field : List.of("waiting", "waiters")) {
                if (given.has(field) && given.get(field).isArray()) {
                    for (JsonNode session : given.get(field)) {
                        state.enqueue(
                                session.isObject()
                                        ? waiter(
                                                session.required("node").asText(),
                                                1,
                                                session.required("sessionRid").asText())
                                        : waiter("n", 1, session.asText()));
                    }
                }
            }
            if (given.has("reports")) {
                for (JsonNode report : given.get("reports")) {
                    state.seedNode(
                            AdmissionQueueFixture.id(report.required("node").asText()),
                            1,
                            report.required("limit").asInt(),
                            report.required("inFlight").asInt(),
                            report.required("issuedSinceReport").asInt());
                }
            }
            if (given.has("reportAgeMs")) {
                state.seedNode(AdmissionQueueFixture.id("n"), 1, 10, 0, 0);
                clock.addAndGet(given.get("reportAgeMs").asLong());
            }
            if (given.has("validTickets")) {
                for (JsonNode valid : given.get("validTickets")) {
                    state.seedTicket(
                            waiter(
                                    valid.required("node").asText(),
                                    1,
                                    valid.required("sessionRid").asText()));
                }
            }
            if (given.has("invalidatedTickets")) {
                for (JsonNode invalid : given.get("invalidatedTickets")) {
                    invalidated.add(invalid.required("sessionRid").asText());
                }
            }
            if (given.has("issuedTickets")) {
                for (JsonNode issued : given.get("issuedTickets")) {
                    String ticket =
                            state.seedTicket(
                                    waiter(
                                            issued.required("node").asText(),
                                            1,
                                            issued.required("sessionRid").asText()));
                    ticketAliases.put(issued.required("ticket").asText(), ticket);
                }
            }
            if (given.has("runningPermits") && given.get("runningPermits").isNumber()) {
                for (int count = 0; count < given.get("runningPermits").asInt(); count++) {
                    lastPermit = nextSeededPermit++;
                    state.seedPermit(lastPermit, "seed", 1, "seed-owner", 1, 0);
                }
            } else if (given.has("runningPermits")) {
                for (JsonNode seeded : given.get("runningPermits")) {
                    long id = nextSeededPermit++;
                    if (seeded.has("permit")) {
                        permitAliases.put(seeded.get("permit").asText(), id);
                    }
                    state.seedPermit(
                            id,
                            AdmissionQueueFixture.id(seeded.required("node").asText()),
                            1,
                            seeded.required("ownerId").asText(),
                            seeded.required("leaseGeneration").asLong(),
                            AdmissionQueueFixture.number(seeded, "requestSeq", 0));
                }
            }
        }

        void play(JsonNode steps) {
            for (JsonNode step : steps) {
                JsonNode expect = step.required("expect");
                switch (step.required("action").asText()) {
                    case "enqueue" -> enqueue(step, expect);
                    case "issueTicket" -> issueTicket(expect);
                    case "nodeReport" -> nodeReport(step, expect);
                    case "calculateBudget" ->
                            assertEquals(
                                    expect.required("budget").asInt(),
                                    state.budget(AdmissionQueueFixture.id("n"), 1));
                    case "issue" -> issue(expect);
                    case "nodeWaiters" -> {
                        List<String> order = new ArrayList<>();
                        for (Connection waiter : state.waitersInOrder()) {
                            if (waiter.nodeRid()
                                    .equals(
                                            AdmissionQueueFixture.id(
                                                    step.required("node").asText()))) {
                                order.add(name(waiter.sessionRid()));
                            }
                        }
                        List<String> expected = new ArrayList<>();
                        expect.required("order").forEach(s -> expected.add(s.asText()));
                        assertEquals(expected, order);
                    }
                    case "admitPermit" -> admitPermit(step, expect);
                    case "releasePermit" -> {
                        state.releasePermit(lastPermit);
                        assertEquals(
                                expect.required("runningPermits").asInt(), state.runningPermits());
                    }
                    case "ownerLeaseEnded" -> {
                        state.ownerLeaseEnded(
                                step.required("ownerId").asText(),
                                step.required("leaseGeneration").asLong());
                        assertEquals(
                                expect.required("runningPermits").asInt(), state.runningPermits());
                    }
                    case "estimate" ->
                            assertEquals(
                                    expect.required("estimatedWaitMs").asLong(),
                                    ZLinkAdmissionQueueState.estimatedWaitMs(
                                            step.required("position").asLong(),
                                            step.required("ticketsLast30Seconds").asLong()));
                    case "ticketExpires" -> ticketExpires(step, expect);
                    case "reinitialize" -> reinitialize(expect);
                    case "verifyOldTicket" -> verifyOldTicket(step, expect);
                    default ->
                            throw new AssertionError(
                                    "unknown action: " + step.required("action").asText());
                }
            }
        }

        private void enqueue(JsonNode step, JsonNode expect) {
            Connection waiter =
                    waiter(
                            AdmissionQueueFixture.text(
                                    step,
                                    "node",
                                    AdmissionQueueFixture.text(given, "nodeRid", "n")),
                            AdmissionQueueFixture.number(step, "nodeLifecycleGeneration", 1),
                            AdmissionQueueFixture.text(
                                    step,
                                    "sessionRid",
                                    AdmissionQueueFixture.text(given, "sessionRid", "a")),
                            AdmissionQueueFixture.text(step, "streamName", STREAM));
            int before = state.waitersInOrder().size();
            var reply = state.enqueue(waiter);
            assertEquals(
                    expect.required("newWaiters").asInt(), state.waitersInOrder().size() - before);
            if (expect.has("position")) {
                assertEquals(
                        expect.get("position").asInt(), reply.delivery().position().intValue());
            }
            if (expect.has("ticket")) {
                assertEquals(
                        ticketAliases.get(expect.get("ticket").asText()),
                        reply.delivery().ticket());
            }
        }

        private void issueTicket(JsonNode expect) {
            state.seedNode(AdmissionQueueFixture.id("n"), 1, 10, 0, 0);
            state.issueNow(AdmissionQueueFixture.id("n"), 1);
            String ticket = state.ticketOf(waiter("n", 1, given.required("sessionRid").asText()));
            assertNotNull(ticket);
            ticketAliases.put(expect.required("ticket").asText(), ticket);
        }

        private void nodeReport(JsonNode step, JsonNode expect) {
            String node = AdmissionQueueFixture.text(step, "node", "n");
            long generation = AdmissionQueueFixture.number(step, "nodeLifecycleGeneration", 1);
            List<ConnectionRef> used = new ArrayList<>();
            for (JsonNode ref : step.path("usedTickets")) {
                used.add(
                        new ConnectionRef(
                                ref.required("streamName").asText(),
                                AdmissionQueueFixture.id(ref.required("sessionRid").asText())));
            }
            for (JsonNode session : step.path("usedTicketSessions")) {
                used.add(new ConnectionRef(STREAM, AdmissionQueueFixture.id(session.asText())));
            }
            List<ConnectionRef> ended = new ArrayList<>();
            for (JsonNode session : step.path("endedSessions")) {
                ended.add(new ConnectionRef(STREAM, AdmissionQueueFixture.id(session.asText())));
            }
            List<Long> held = new ArrayList<>();
            for (JsonNode alias : step.path("heldPermits")) {
                held.add(permitAliases.get(alias.asText()));
            }
            state.report(
                    new NodeReport(
                            AdmissionQueueFixture.id(node),
                            generation,
                            0,
                            0,
                            used,
                            ended,
                            held,
                            AdmissionQueueFixture.number(step, "resolvedRequestSeq", 0)));
            if (expect.has("runningPermits")) {
                assertEquals(expect.get("runningPermits").asInt(), state.runningPermits());
            }
            if (expect.has("waiting")) {
                List<String> expected = new ArrayList<>();
                for (JsonNode entry : expect.get("waiting")) {
                    expected.add(
                            entry.required("node").asText()
                                    + "/"
                                    + entry.required("sessionRid").asText());
                }
                List<String> actual = new ArrayList<>();
                for (Connection waiter : state.waitersInOrder()) {
                    actual.add(name(waiter.nodeRid()) + "/" + name(waiter.sessionRid()));
                }
                assertEquals(expected, actual);
            }
            if (expect.has("waiters")) {
                List<String> expected = new ArrayList<>();
                expect.get("waiters").forEach(session -> expected.add(session.asText()));
                assertEquals(expected, sessions());
            }
        }

        private void issue(JsonNode expect) {
            String node = "n";
            if (!state.waitersInOrder().isEmpty()) {
                node = name(state.waitersInOrder().getFirst().nodeRid());
            }
            if (!given.has("reports") && !given.has("reportAgeMs")) {
                state.seedNode(AdmissionQueueFixture.id(node), 1, 10, 0, 0);
            }
            List<String> issued = new ArrayList<>();
            state.issueNow(AdmissionQueueFixture.id(node), 1)
                    .forEach(waiter -> issued.add(name(waiter.sessionRid())));
            List<String> expectedIssued = new ArrayList<>();
            expect.required("ticketSessions").forEach(s -> expectedIssued.add(s.asText()));
            assertEquals(expectedIssued, issued);
            if (expect.has("remaining")) {
                List<String> remaining = new ArrayList<>();
                for (Connection waiter : state.waitersInOrder()) {
                    if (state.ticketOf(waiter) == null) {
                        remaining.add(name(waiter.sessionRid()));
                    }
                }
                List<String> expectedRemaining = new ArrayList<>();
                expect.get("remaining").forEach(s -> expectedRemaining.add(s.asText()));
                assertEquals(expectedRemaining, remaining);
            }
        }

        private void admitPermit(JsonNode step, JsonNode expect) {
            // A requesting connection holds a valid ticket unless the scenario invalidated it.
            Connection holder =
                    waiter(step.required("node").asText(), 1, step.required("sessionRid").asText());
            if (!given.has("validTickets")
                    && !invalidated.contains(step.required("sessionRid").asText())
                    && state.ticketOf(holder) == null) {
                state.seedTicket(holder);
            }
            var reply =
                    state.admitPermit(
                            new AdmitPermit(
                                    AdmissionQueueFixture.id(step.required("node").asText()),
                                    1L,
                                    step.required("ownerId").asText(),
                                    step.required("leaseGeneration").asLong(),
                                    STREAM,
                                    AdmissionQueueFixture.id(step.required("sessionRid").asText()),
                                    step.required("epoch").asLong(),
                                    step.required("requestSeq").asLong()));
            assertEquals(expect.required("granted").asBoolean(), reply.granted());
            if (reply.granted()) {
                assertNotEquals(0, reply.permitId());
                lastPermit = reply.permitId();
                if (step.has("permit")) {
                    permitAliases.put(step.get("permit").asText(), reply.permitId());
                }
            } else {
                assertEquals(0, reply.permitId());
            }
            if (expect.has("permitIdNonzero")) {
                assertEquals(expect.get("permitIdNonzero").asBoolean(), reply.permitId() != 0);
            }
            assertEquals(expect.required("runningPermits").asInt(), state.runningPermits());
        }

        private void ticketExpires(JsonNode step, JsonNode expect) {
            state.seedNode(AdmissionQueueFixture.id("n"), 1, 10, 0, 0);
            state.issueNow(AdmissionQueueFixture.id("n"), 1);
            Connection waiter = waiter("n", 1, step.required("sessionRid").asText());
            assertNotNull(state.ticketOf(waiter));
            clock.addAndGet(ZLinkAdmissionQueueState.TICKET_LIFETIME_MS + 1);
            state.sweep();
            List<String> expected = new ArrayList<>();
            expect.required("waiters").forEach(session -> expected.add(session.asText()));
            assertEquals(expected, sessions());
        }

        private void reinitialize(JsonNode expect) {
            epoch++;
            state = new ZLinkAdmissionQueueState(tickets, globalLimit, clock::get, epoch);
            assertEquals(expect.required("epoch").asLong(), state.epoch());
            assertTrue(state.waitersInOrder().isEmpty());
        }

        private void verifyOldTicket(JsonNode step, JsonNode expect) {
            var connection = ZLinkAdmissionQueueState.connection(waiter("n", 1, "a"));
            String old =
                    tickets.issue(
                            connection,
                            clock.get() + ZLinkAdmissionQueueState.TICKET_LIFETIME_MS,
                            step.required("ticketEpoch").asLong());
            boolean accepted = tickets.verifies(old, connection, epoch, clock.get());
            assertEquals(expect.required("accept").asBoolean(), accepted);
            assertFalse(accepted);
        }

        private List<String> sessions() {
            List<String> sessions = new ArrayList<>();
            state.waitersInOrder().forEach(waiter -> sessions.add(name(waiter.sessionRid())));
            return sessions;
        }
    }

    private static Connection waiter(String node, long generation, String session) {
        return waiter(node, generation, session, STREAM);
    }

    private static Connection waiter(String node, long generation, String session, String stream) {
        return new Connection(
                AdmissionQueueFixture.id(node),
                generation,
                stream,
                AdmissionQueueFixture.id(session));
    }

    private static String name(String encoded) {
        return new String(Base64.getUrlDecoder().decode(encoded), StandardCharsets.UTF_8);
    }
}
