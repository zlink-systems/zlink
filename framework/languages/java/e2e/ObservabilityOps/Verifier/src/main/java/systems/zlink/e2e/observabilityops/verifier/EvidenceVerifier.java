package systems.zlink.e2e.observabilityops.verifier;

import com.fasterxml.jackson.databind.JsonNode;
import com.fasterxml.jackson.databind.ObjectMapper;
import java.io.IOException;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.ArrayList;
import java.util.HashSet;
import java.util.List;
import java.util.Set;
import java.util.function.Predicate;

/** Verifies only evidence emitted by deployed framework processes. */
public final class EvidenceVerifier {
    private static final ObjectMapper JSON = new ObjectMapper();
    private static final Set<String> FORBIDDEN_METRIC_TAGS = Set.of(
        "correlation_id", "flow_id", "actor_id", "spot_rid");
    private static final Set<String> CLOSE_REASONS = Set.of(
        "client_close", "idle_timeout", "heartbeat_timeout", "server_drain",
        "protocol_error", "transport_error");

    public List<String> verify(Path evidenceDirectory, String selector) throws IOException {
        List<String> selected = scenarioIds(selector);
        List<String> passed = new ArrayList<>();
        for (String id : selected) {
            Path path = evidenceDirectory.resolve(id + ".json");
            ensure(Files.isRegularFile(path), id + " evidence file is missing: " + path);
            JsonNode evidence = JSON.readTree(path.toFile());
            ensure(id.equals(evidence.path("scenario").asText()), id + " scenario marker mismatch");
            switch (id) {
                case "OBS-A1" -> verifyA1(evidence);
                case "OBS-A2" -> verifyA2(evidence);
                case "OBS-A3" -> verifyA3(evidence);
                case "OBS-A4" -> verifyA4(evidence);
                case "OBS-B1" -> verifyB1(evidence);
                case "OBS-B2" -> verifyB2(evidence);
                case "OBS-B3" -> verifyB3(evidence);
                case "OBS-B4" -> verifyB4(evidence);
                case "OBS-C1" -> verifyC1(evidence);
                case "OBS-C2" -> verifyC2(evidence);
                case "OBS-C3" -> verifyC3(evidence);
                case "OBS-C4" -> verifyC4(evidence);
                case "OBS-C5" -> verifyC5(evidence);
                default -> throw new IllegalArgumentException("Unknown scenario " + id);
            }
            passed.add(id);
        }
        return List.copyOf(passed);
    }

    static List<String> scenarioIds(String selector) {
        if (selector == null || selector.isBlank() || "all".equalsIgnoreCase(selector)) {
            return List.of(
                "OBS-A1", "OBS-A2", "OBS-A3", "OBS-A4",
                "OBS-B1", "OBS-B2", "OBS-B3", "OBS-B4",
                "OBS-C1", "OBS-C2", "OBS-C3", "OBS-C4", "OBS-C5");
        }
        String normalized = selector.toUpperCase();
        if (!scenarioIds("all").contains(normalized)) {
            throw new IllegalArgumentException("Unknown Config 11 selector '" + selector + "'");
        }
        return List.of(normalized);
    }

    private static void verifyA1(JsonNode root) {
        JsonNode flows = array(root, "flows");
        String flow = oneText(flows, "flow");
        requireOrderedPhases(flows, flow,
            "connector_outbound", "stream_inbound", "actor_relay", "spot_dispatch");
    }

    private static void verifyA2(JsonNode root) {
        JsonNode flows = array(root, "flows");
        String flow = oneText(flows, "flow");
        ensure(any(flows, row -> flow.equals(text(row, "flow"))
            && "received".equals(text(row, "outcome"))), "OBS-A2 received line missing");
        ensure(any(flows, row -> flow.equals(text(row, "flow"))
            && "error".equals(text(row, "outcome"))), "OBS-A2 error line with same flow missing");
    }

    private static void verifyA3(JsonNode root) {
        JsonNode flows = array(root, "flows");
        String flow = oneText(flows, "flow");
        ensure(any(flows, row -> "entry".equals(text(row, "phase"))), "OBS-A3 entry missing");
        ensure(any(flows, row -> "after_off_node".equals(text(row, "phase"))
            && flow.equals(text(row, "flow"))), "OBS-A3 downstream propagation missing");
        ensure(!any(flows, row -> "off-node".equals(text(row, "label"))),
            "OBS-A3 off node emitted a flow event");
    }

    private static void verifyA4(JsonNode root) {
        JsonNode flows = array(root, "flows");
        String publishFlow = requiredText(root, "publishFlow");
        long subscribers = count(flows, row -> publishFlow.equals(text(row, "flow"))
            && ("subscriber".equals(text(row, "phase")) || "owner_skip".equals(text(row, "phase"))));
        ensure(subscribers >= 2, "OBS-A4 fan-out branches are missing");
        ensure(any(flows, row -> "timer".equals(text(row, "origin"))
            && !publishFlow.equals(text(row, "flow"))), "OBS-A4 timer root flow missing");
    }

    private static void verifyB1(JsonNode root) {
        JsonNode metrics = metrics(root);
        metric(metrics, "zlink.stream.connections.active", "updown", "{connection}", 0);
        metric(metrics, "zlink.stream.connections.opened", "counter", "{connection}", 3);
        metric(metrics, "zlink.stream.connections.closed", "counter", "{connection}", 3);
        metric(metrics, "zlink.stream.reconnects", "counter", "{event}", 1);
        for (JsonNode row : metrics) {
            if ("zlink.stream.connections.closed".equals(text(row, "name"))) {
                ensure(CLOSE_REASONS.contains(tag(row, "close_reason")),
                    "OBS-B1 close_reason is not in the closed set");
            }
        }
    }

    private static void verifyB2(JsonNode root) {
        JsonNode metrics = metrics(root);
        metric(metrics, "zlink.actor.transfers", "counter", "{transfer}", 1);
        metric(metrics, "zlink.actor.transfer.duration", "histogram", "s", 1);
        JsonNode pending = metric(metrics,
            "zlink.actor.transfer.pending_requests.count", "histogram", "{request}", 1);
        ensure(pending.path("count").asLong() == 1,
            "OBS-B2 pending request snapshot must be recorded once per transfer");
    }

    private static void verifyB3(JsonNode root) {
        JsonNode metrics = metrics(root);
        ensure(findMetric(metrics, "zlink.fanout.published") == null,
            "OBS-B3 publish-specific metrics must not be registered");
        metric(metrics, "zlink.location.owner_lease.renew.lateness", "histogram", "s", 1);
        assertNoHighCardinalityTags(metrics);
        if (!root.path("dropObservable").asBoolean(false)) {
            ensure(findMetric(metrics, "zlink.fanout.dropped") == null,
                "OBS-B3 unobservable backend must not register fanout.dropped");
        }
    }

    private static void verifyB4(JsonNode root) {
        ensure(root.path("messagingAccurate").asBoolean(), "OBS-B4 messaging changed without a reader");
        ensure(root.path("metricSeriesStored").asLong(-1) == 0,
            "OBS-B4 reader-free runtime retained metric series");
        ensure(root.path("trafficEvents").asLong() >= 8192,
            "OBS-B4 traffic sample is too small to prove bounded storage");
    }

    private static void verifyC1(JsonNode root) {
        ensure(!root.path("readyAfterDrain").asBoolean(true), "OBS-C1 readiness did not flip");
        ensure(root.path("existingRequestSucceeded").asBoolean(), "OBS-C1 existing request failed");
        ensure(root.path("leaseRenewedWhileDraining").asBoolean(), "OBS-C1 owner lease stopped renewing");
        ensure(any(array(root, "peerRows"), row -> row.path("draining").asBoolean()),
            "OBS-C1 typed draining peer row missing");
        requireDrainStates(root, "serving", "draining");
    }

    private static void verifyC2(JsonNode root) {
        ensure(root.path("locationTakenOver").asBoolean(), "OBS-C2 location takeover missing");
        ensure(root.path("boundPushContinued").asBoolean(), "OBS-C2 bound push did not continue");
        metric(metrics(root), "zlink.drain.actors.handed_off", "counter", "{actor}", 1);
        ensure(root.path("pendingRequestsCompleted").asBoolean(),
            "OBS-C2 pending requests did not preserve reply or timeout");
    }

    private static void verifyC3(JsonNode root) {
        ensure(root.path("fixedDrainCompleted").asBoolean(),
            "OBS-C3 fixed drain did not complete");
        ensure(root.path("ownerReleasedAndRecreated").asBoolean(),
            "OBS-C3 room was not rebuilt on another node");
        ensure(root.path("replayRestoredState").asBoolean(), "OBS-C3 replay did not restore state");
        metric(metrics(root), "zlink.drain.rooms.drained", "counter", "{room}", 1);
    }

    private static void verifyC4(JsonNode root) {
        requireDrainStates(root, "draining", "force_stopping");
        ensure("force_stopped".equals(requiredText(root, "result")), "OBS-C4 terminal result mismatch");
        ensure("deadline_exceeded".equals(requiredText(root, "reason")), "OBS-C4 reason mismatch");
        ensure("server_drain".equals(requiredText(root, "closeReason")), "OBS-C4 close reason mismatch");
        ensure(root.path("sessionClosingVersion").asInt() == 1, "OBS-C4 control version mismatch");
        metricWithTag(metrics(root), "zlink.drain.forced", "kind", "session", 1);
    }

    private static void verifyC5(JsonNode root) {
        ensure("drained".equals(root.path("rolloutResult").asText()),
            "OBS-C5 rollout with a target did not drain cleanly");
        ensure(!root.path("rolloutForceStopping").asBoolean(),
            "OBS-C5 rollout entered force stopping");
        ensure(root.path("zeroTargetHeldAtSource").asBoolean(),
            "OBS-C5 zero-target actor left its source");
        ensure("force_stopped".equals(root.path("zeroTargetResult").asText()),
            "OBS-C5 zero-target terminal result mismatch");
        ensure("deadline_exceeded".equals(root.path("zeroTargetReason").asText()),
            "OBS-C5 zero-target reason mismatch");
    }

    private static JsonNode metrics(JsonNode root) {
        JsonNode metrics = array(root, "metrics");
        assertNoHighCardinalityTags(metrics);
        return metrics;
    }

    private static JsonNode metric(
        JsonNode metrics, String name, String kind, String unit, double minimum) {
        JsonNode row = findMetric(metrics, name);
        ensure(row != null, "metric missing: " + name);
        ensure(kind.equals(text(row, "kind")), name + " kind mismatch");
        ensure(unit.equals(text(row, "unit")), name + " unit mismatch");
        ensure(row.path("value").asDouble() >= minimum, name + " value is below " + minimum);
        return row;
    }

    private static JsonNode metricWithTag(
        JsonNode metrics, String name, String key, String value, double minimum) {
        for (JsonNode row : metrics) {
            if (name.equals(text(row, "name")) && value.equals(tag(row, key))) {
                ensure(row.path("value").asDouble() >= minimum, name + " value is below " + minimum);
                return row;
            }
        }
        throw new IllegalStateException(name + " with " + key + "=" + value + " is missing");
    }

    private static JsonNode findMetric(JsonNode metrics, String name) {
        for (JsonNode row : metrics) if (name.equals(text(row, "name"))) return row;
        return null;
    }

    private static void assertNoHighCardinalityTags(JsonNode metrics) {
        for (JsonNode row : metrics) {
            JsonNode tags = row.path("tags");
            for (String forbidden : FORBIDDEN_METRIC_TAGS) {
                ensure(!tags.has(forbidden), text(row, "name") + " has forbidden tag " + forbidden);
            }
        }
    }

    private static void requireDrainStates(JsonNode root, String... expected) {
        Set<String> actual = new HashSet<>();
        for (JsonNode event : array(root, "drainEvents")) actual.add(text(event, "state"));
        for (String state : expected) ensure(actual.contains(state), "drain state missing: " + state);
    }

    private static void requireOrderedPhases(JsonNode rows, String flow, String... phases) {
        long previous = Long.MIN_VALUE;
        for (String phase : phases) {
            long sequence = Long.MIN_VALUE;
            for (JsonNode row : rows) {
                if (flow.equals(text(row, "flow")) && phase.equals(text(row, "phase"))) {
                    sequence = row.path("sequence").asLong(Long.MIN_VALUE);
                    break;
                }
            }
            ensure(sequence > previous, "flow phase is missing or out of order: " + phase);
            previous = sequence;
        }
    }

    private static String oneText(JsonNode rows, String field) {
        Set<String> values = new HashSet<>();
        for (JsonNode row : rows) {
            String value = text(row, field);
            if (!value.isBlank()) values.add(value);
        }
        ensure(values.size() == 1, field + " must have one byte-identical value but was " + values);
        return values.iterator().next();
    }

    private static JsonNode array(JsonNode root, String name) {
        JsonNode value = root.path(name);
        ensure(value.isArray(), name + " must be an array");
        return value;
    }

    private static String requiredText(JsonNode root, String name) {
        String value = text(root, name);
        ensure(!value.isBlank(), name + " is required");
        return value;
    }

    private static String text(JsonNode root, String name) {
        return root.path(name).asText("");
    }

    private static String tag(JsonNode metric, String name) {
        return metric.path("tags").path(name).asText("");
    }

    private static boolean any(JsonNode rows, Predicate<JsonNode> predicate) {
        return count(rows, predicate) > 0;
    }

    private static long count(JsonNode rows, Predicate<JsonNode> predicate) {
        long count = 0;
        for (JsonNode row : rows) if (predicate.test(row)) count++;
        return count;
    }

    private static void ensure(boolean condition, String message) {
        if (!condition) throw new IllegalStateException(message);
    }
}
