package systems.zlink.e2e.kotlin.toactormessaging.client;

import systems.zlink.e2e.kotlin.toactormessaging.shared.Contracts;
import systems.zlink.e2e.kotlin.toactormessaging.shared.Env;
import systems.zlink.e2e.kotlin.toactormessaging.shared.JsonHttp;

public final class Program {
    private Program() {
    }

    public static void main(String... args) {
        Env.configure(args);
        String actorUrl = Env.get("actorHttpEndpoint");
        String callerUrl = Env.get("callerHttpEndpoint");

        ensureReady(actorUrl, callerUrl, "TA-A1", "ta-a1");
        assertCall(callerUrl, "TA-A1-send", "ta-a1", "a1-send", "sent", true);
        assertCall(callerUrl, "TA-A1-request", "ta-a1", "a1-request", "reply:a1-request", false);

        ensureReady(actorUrl, callerUrl, "TA-A2", "ta-a2");
        assertCall(callerUrl, "TA-A2-send", "ta-a2", "a2-send", "sent", true);
        assertCall(callerUrl, "TA-A2-request", "ta-a2", "a2-request", "reply:a2-request", false);

        assertFailure(callerUrl, "TA-A3-before-bind", "ta-a3-missing", "ACTOR_ROUTE_NOT_FOUND", false);
        ensureReady(actorUrl, callerUrl, "TA-A3", "ta-a3");
        assertCall(callerUrl, "TA-A3-after-bind-send", "ta-a3", "a3-send", "sent", true);
        assertCall(callerUrl, "TA-A3-after-bind-request", "ta-a3", "a3-request", "reply:a3-request", false);

        ensureReady(actorUrl, callerUrl, "TA-A4", "ta-a4");
        assertCall(callerUrl, "TA-A4-disconnected-send", "ta-a4", "a4-send", "sent", true);
        assertCall(callerUrl, "TA-A4-disconnected-request", "ta-a4", "a4-request", "reply:a4-request", false);
        assertFailure(callerUrl, "TA-A4-destroyed", "ta-a4-destroyed", "ACTOR_ROUTE_NOT_FOUND", false);

        assertFailure(callerUrl, "TA-B1-missing-send", "missing-actor", "ACTOR_ROUTE_NOT_FOUND", true);
        assertFailure(callerUrl, "TA-B1-missing-request", "missing-actor", "ACTOR_ROUTE_NOT_FOUND", false);

        ensureReady(actorUrl, callerUrl, "TA-B2-live", "ta-b2");
        postFault(actorUrl, "/fault/stale", "TA-B2-stale", "ta-b2");
        assertCall(callerUrl, "TA-B2-stale-cas-rejected", "ta-b2", "b2-stale", "reply:b2-stale", false);
        postFault(actorUrl, "/fault/restore", "TA-B2-restore", "ta-b2");
        assertCall(callerUrl, "TA-B2-live-after-reresolve", "ta-b2", "b2-request", "reply:b2-request", false);

        ensureReady(actorUrl, callerUrl, "TA-B3-live", "ta-b3");
        postFault(actorUrl, "/fault/route-disconnected", "TA-B3-route-disconnected", "ta-b3");
        assertCall(callerUrl, "TA-B3-stale-delete-rejected", "ta-b3", "b3-stale", "reply:b3-stale", false);
        postFault(actorUrl, "/fault/restore", "TA-B3-restore", "ta-b3");
        assertCall(callerUrl, "TA-B3-route-restored", "ta-b3", "b3-request", "reply:b3-request", false);

        System.out.println("to-actor-messaging e2e result=passed");
    }

    private static void ensure(String actorUrl, String scenario, String actorId) {
        JsonHttp.postJson(
            actorUrl + "/ensure",
            new Contracts.ActorCallRequest(scenario, actorId, "ensure"),
            Contracts.ActorCallResponse.class);
    }

    private static void postFault(String actorUrl, String path, String scenario, String actorId) {
        JsonHttp.postJson(
            actorUrl + path,
            new Contracts.ActorFaultRequest(scenario, actorId),
            Contracts.ActorCallResponse.class);
    }

    private static void ensureReady(String actorUrl, String callerUrl, String scenario, String actorId) {
        ensure(actorUrl, scenario, actorId);
        waitUntilReady(callerUrl, scenario + "-ready", actorId);
    }

    private static void waitUntilReady(String callerUrl, String scenario, String actorId) {
        long deadline = System.nanoTime() + 5_000_000_000L;
        Contracts.ActorCallResponse response = null;
        while (System.nanoTime() < deadline) {
            response = call(callerUrl, scenario, actorId, "ready", false);
            if (response.errorKind() == null && "reply:ready".equals(response.result())) {
                return;
            }
            if (!isConvergenceError(response.errorKind())) {
                break;
            }
            sleepBriefly();
        }
        String error = response == null ? "no response" : response.errorKind();
        throw new IllegalStateException(scenario + " readiness failed " + error);
    }

    private static boolean isConvergenceError(String errorKind) {
        return "REQUEST_FAILED".equals(errorKind) || "ACTOR_ROUTE_NOT_FOUND".equals(errorKind);
    }

    private static void sleepBriefly() {
        try {
            Thread.sleep(100);
        } catch (InterruptedException ex) {
            Thread.currentThread().interrupt();
            throw new IllegalStateException("readiness wait interrupted", ex);
        }
    }

    private static void assertCall(
        String callerUrl,
        String scenario,
        String actorId,
        String value,
        String expected,
        boolean send) {
        Contracts.ActorCallResponse response = call(callerUrl, scenario, actorId, value, send);
        require(response.errorKind() == null, scenario + " unexpected error " + response.errorKind());
        require(expected.equals(response.result()), scenario + " expected " + expected + " got " + response.result());
    }

    private static void assertFailure(
        String callerUrl,
        String scenario,
        String actorId,
        String expectedKind,
        boolean send) {
        String endpoint = send ? "/send" : "/request";
        Contracts.ActorCallResponse response = JsonHttp.postJson(
            callerUrl + endpoint,
            new Contracts.ActorCallRequest(scenario, actorId, "missing"),
            Contracts.ActorCallResponse.class);
        require(expectedKind.equals(response.errorKind()),
            scenario + " expected " + expectedKind + " got " + response.errorKind());
    }

    private static Contracts.ActorCallResponse call(
        String callerUrl,
        String scenario,
        String actorId,
        String value,
        boolean send) {
        String endpoint = send ? "/send" : "/request";
        return JsonHttp.postJson(
            callerUrl + endpoint,
            new Contracts.ActorCallRequest(scenario, actorId, value),
            Contracts.ActorCallResponse.class);
    }

    private static void assertNoEvidence(String actorUrl, String scenario) {
        Contracts.ActorEvidence[] evidence = JsonHttp.getJson(
            actorUrl + "/evidence",
            Contracts.ActorEvidence[].class);
        for (Contracts.ActorEvidence row : evidence) {
            require(!scenario.equals(row.scenario()), scenario + " unexpectedly executed actor handler");
        }
    }

    private static void require(boolean condition, String message) {
        if (!condition) {
            throw new IllegalStateException(message);
        }
    }
}
