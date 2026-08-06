package systems.zlink.e2e.observabilityops.a5.client;

import com.fasterxml.jackson.databind.JsonNode;
import com.fasterxml.jackson.databind.ObjectMapper;
import java.time.Duration;
import java.util.function.Predicate;
import systems.zlink.httpclient.RawHttpResponse;
import systems.zlink.httpclient.ZLinkHttpClient;

public final class Program {
    private static final ObjectMapper JSON = new ObjectMapper();
    private final String endpoint;

    private Program(String endpoint) {
        this.endpoint = endpoint;
    }

    public static void main(String... args) throws Exception {
        if (args.length != 2 || !"--endpoint".equals(args[0]) || args[1].isBlank()) {
            throw new IllegalArgumentException("Usage: observability-ops-a5-client --endpoint <url>");
        }
        new Program(args[1]).run();
        System.out.println("scenario OBS-A5 passed");
    }

    private void run() throws Exception {
        setMode("KEY_TRANSITIONS");
        int keyBefore = snapshot().count();
        RawHttpResponse keyReply = request("key-transition", false);
        ensure(keyReply.status() == 200 && keyReply.body().contains("key-transition"),
            "KEY_TRANSITIONS request failed: " + keyReply.body());
        FlowSnapshot keyAfter = waitForEvent(
            keyBefore,
            event -> isProbe(event) && isSuccess(event.path("outcome").asText()),
            "KEY_TRANSITIONS did not produce success evidence");

        setMode("OFF");
        int offBefore = snapshot().count();
        RawHttpResponse offReply = request("off", false);
        ensure(offReply.status() == 200 && offReply.body().contains("\"value\":\"off\""),
            "OFF request failed: " + offReply.body());
        waitForCount(offBefore, "OFF produced flow evidence for a success request");

        setMode("ERRORS_ONLY");
        int errorBefore = snapshot().count();
        RawHttpResponse normalReply = request("errors-only-normal", false);
        ensure(normalReply.status() == 200,
            "ERRORS_ONLY normal request failed: " + normalReply.body());
        waitForCount(errorBefore, "ERRORS_ONLY produced success evidence");
        RawHttpResponse failureReply = request("errors-only-failure", true);
        ensure(failureReply.status() >= 500,
            "ERRORS_ONLY failure did not fail: " + failureReply.body());
        FlowSnapshot errorAfter = waitForEvent(
            errorBefore,
            event -> isProbe(event)
                && "ERROR".equals(event.path("outcome").asText())
                && (!event.path("errorType").asText().isBlank()
                    || !event.path("errorReason").isMissingNode()),
            "ERRORS_ONLY did not produce error evidence");

        setMode("KEY_TRANSITIONS");
        int finalBefore = snapshot().count();
        RawHttpResponse finalReply = request("key-transition-again", false);
        ensure(finalReply.status() == 200,
            "final KEY_TRANSITIONS request failed: " + finalReply.body());
        FlowSnapshot finalAfter = waitForEvent(
            finalBefore,
            event -> isProbe(event) && isSuccess(event.path("outcome").asText()),
            "KEY_TRANSITIONS did not resume success evidence");
        System.out.println("OBS-A5 evidence key=" + keyAfter.count()
            + " error=" + errorAfter.count()
            + " resumed=" + finalAfter.count());
    }

    private void setMode(String mode) throws Exception {
        RawHttpResponse response = post("/mode?value=" + mode);
        ensure(response.status() == 200 && response.body().contains("\"mode\":\"" + mode + "\""),
            "mode change was not acknowledged: " + response.body());
    }

    private RawHttpResponse request(String value, boolean fail) {
        return post("/request?value=" + value + "&fail=" + fail);
    }

    private RawHttpResponse post(String path) {
        return ZLinkHttpClient.create(endpoint)
            .timeout(Duration.ofSeconds(10))
            .post(path)
            .submitRaw()
            .toCompletableFuture()
            .join();
    }

    private FlowSnapshot snapshot() throws Exception {
        RawHttpResponse response = ZLinkHttpClient.create(endpoint)
            .timeout(Duration.ofSeconds(5))
            .get("/flows?after=0")
            .submitRaw()
            .toCompletableFuture()
            .join();
        ensure(response.status() == 200, "flow endpoint failed: " + response.body());
        JsonNode root = JSON.readTree(response.body());
        return new FlowSnapshot(root.path("count").asInt(), root.path("events"));
    }

    private FlowSnapshot waitForEvent(
        int after,
        Predicate<JsonNode> predicate,
        String message) throws Exception {
        long deadline = System.nanoTime() + Duration.ofSeconds(10).toNanos();
        FlowSnapshot last = snapshot();
        while (System.nanoTime() < deadline) {
            last = snapshot();
            if (last.count() > after) {
                for (JsonNode event : last.events()) {
                    if (predicate.test(event)) {
                        return last;
                    }
                }
            }
            Thread.sleep(50);
        }
        throw new IllegalStateException(message + "; events=" + last.events());
    }

    private void waitForCount(int expected, String message) throws Exception {
        long deadline = System.nanoTime() + Duration.ofSeconds(5).toNanos();
        FlowSnapshot last = snapshot();
        while (System.nanoTime() < deadline) {
            last = snapshot();
            if (last.count() == expected) {
                return;
            }
            Thread.sleep(50);
        }
        throw new IllegalStateException(message + "; expected=" + expected
            + ", actual=" + last.count());
    }

    private static boolean isProbe(JsonNode event) {
        String packet = event.path("packetName").asText();
        return packet.contains("Request") || packet.contains("Probe");
    }

    private static boolean isSuccess(String outcome) {
        return "SENT".equals(outcome)
            || "REPLY_RECEIVED".equals(outcome)
            || "RECEIVED".equals(outcome)
            || "DISPATCHED".equals(outcome)
            || "REPLIED".equals(outcome);
    }

    private static void ensure(boolean condition, String message) {
        if (!condition) {
            throw new IllegalStateException(message);
        }
    }

    private record FlowSnapshot(int count, JsonNode events) {
    }
}
