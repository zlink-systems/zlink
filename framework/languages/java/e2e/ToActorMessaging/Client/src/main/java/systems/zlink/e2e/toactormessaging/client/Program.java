package systems.zlink.e2e.toactormessaging.client;
import java.util.ArrayList;
import java.util.Set;
import java.util.stream.Stream;

import java.net.URI;
import java.time.Duration;
import java.util.List;
import systems.zlink.e2e.toactormessaging.shared.Contracts;
import systems.zlink.stream.connector.ZLinkStreamConnector;
import systems.zlink.stream.connector.ZLinkStreamConnectorFactory;
import systems.zlink.stream.connector.ZLinkStreamConnectorOptions;
import systems.zlink.stream.connector.ZLinkStreamCompression;
import systems.zlink.stream.connector.ZLinkStreamDispatchMode;
import systems.zlink.stream.connector.ZLinkStreamPacketNameResolver;

public final class Program {
    private Program() {
    }

    public static void main(String... args) {
        require(args.length == 4 && "--config".equals(args[0]) && "--scenario".equals(args[2]),
            "Usage: Client --config <path> --scenario <selector>");
        ClientOptions options = ClientOptions.load(args[1]);
        String actorUrl = options.actorHttpEndpoint();
        String actorBUrl = options.actorBHttpEndpoint();
        String callerUrl = options.callerHttpEndpoint();
        String sessionAUrl = options.sessionAHttpEndpoint();
        String sessionBUrl = options.sessionBHttpEndpoint();
        String sessionAStream = options.sessionAStreamEndpoint();
        String sessionBStream = options.sessionBStreamEndpoint();
        String selector = args[3];
        require(Set.of("all", "TA-A1", "TA-A2", "TA-A3", "TA-A4",
            "TA-B1", "TA-B2", "TA-B3").contains(selector),
            "unknown ToActorMessaging selector: " + selector);

        if (selected(selector, "TA-A1")) {
            Contracts.ActorRefWire actorRef = ensureRef(actorUrl, "TA-A1", "ta-a1");
            waitUntilReady(callerUrl, "TA-A1-ready", actorRef.actorId());
            ZLinkStreamConnector connector = connectAndBind(sessionAStream, actorRef);
            try {
                assertBoundPush(connector, actorUrl, "TA-A1-before", "ta-a1", "Before");
                assertCall(callerUrl, "TA-A1-send", "ta-a1", "a1-send", "sent", true);
                assertCall(callerUrl, "TA-A1-request", "ta-a1", "a1-request", "reply:a1-request", false);
                assertBoundPush(connector, actorUrl, "TA-A1-after", "ta-a1", "After");
            } finally {
                close(connector);
            }
            assertSessionEvidence(sessionAUrl, "ta-a1", "actor-bound", "TA-A1 bind evidence missing");
        }

        if (selected(selector, "TA-A2")) {
            ensureReady(actorUrl, callerUrl, "TA-A2", "ta-a2");
            assertCall(callerUrl, "TA-A2-send", "ta-a2", "a2-send", "sent", true);
            assertCall(callerUrl, "TA-A2-request", "ta-a2", "a2-request", "reply:a2-request", false);
            assertBoundPushFailure(actorUrl, "TA-A2-push", "ta-a2", "Unbound", "NOT_CONFIGURED");
            assertNoSessionBinding(sessionAUrl, sessionBUrl, "ta-a2", "TA-A2 actor was unexpectedly bound");
        }

        if (selected(selector, "TA-A3")) {
            ensureReady(actorUrl, callerUrl, "TA-A3", "ta-a3");
            assertCall(callerUrl, "TA-A3-before-bind-send", "ta-a3", "a3-before-send", "sent", true);
            assertCall(callerUrl, "TA-A3-before-bind-request", "ta-a3", "a3-before-request",
                "reply:a3-before-request", false);
            assertBoundPushFailure(actorUrl, "TA-A3-before-bind-push", "ta-a3", "BeforeBind",
                "NOT_CONFIGURED");
            Contracts.ActorRefWire actorRef = ensureRef(actorUrl, "TA-A3", "ta-a3");
            ZLinkStreamConnector connector = connectAndBind(sessionBStream, actorRef);
            try {
                assertCall(callerUrl, "TA-A3-after-bind-send", "ta-a3", "a3-send", "sent", true);
                assertCall(callerUrl, "TA-A3-after-bind-request", "ta-a3", "a3-request",
                    "reply:a3-request", false);
                assertBoundPush(connector, actorUrl, "TA-A3-after-bind-push", "ta-a3", "LateBind");
            } finally {
                close(connector);
            }
            assertSessionEvidence(sessionBUrl, "ta-a3", "actor-bound", "TA-A3 late bind evidence missing");
        }

        if (selected(selector, "TA-A4")) {
            Contracts.ActorRefWire actorRef = ensureRef(actorUrl, "TA-A4", "ta-a4");
            waitUntilReady(callerUrl, "TA-A4-ready", actorRef.actorId());
            ZLinkStreamConnector connector = connectAndBind(sessionAStream, actorRef);
            assertBoundPush(connector, actorUrl, "TA-A4-before-disconnect", "ta-a4", "BeforeDisconnect");
            Contracts.UnbindActorRes unbound = ToActorHttpClient.postJson(
                actorUrl + "/unbind", new Contracts.UnbindActorReq("TA-A4-unbind", "ta-a4"),
                Contracts.UnbindActorRes.class);
            require(unbound.unbound(), "TA-A4 unbind was not acknowledged");
            assertSessionEvidence(sessionAUrl, "ta-a4", "actor-bound", "TA-A4 bind evidence missing");
            assertBoundPushFailure(actorUrl, "TA-A4-after-disconnect-push", "ta-a4", "AfterDisconnect",
                "NOT_CONFIGURED");
            assertCall(callerUrl, "TA-A4-disconnected-send", "ta-a4", "a4-send", "sent", true);
            assertCall(callerUrl, "TA-A4-disconnected-request", "ta-a4", "a4-request", "reply:a4-request", false);
            close(connector);
            Contracts.DestroyActorRes destroyed = ToActorHttpClient.postJson(
                actorUrl + "/destroy", new Contracts.DestroyActorReq("TA-A4-destroy", "ta-a4"),
                Contracts.DestroyActorRes.class);
            require(destroyed.destroyed(), "TA-A4 destroy was not acknowledged");
            waitUntilMissing(callerUrl, "TA-A4-destroyed", "ta-a4");
        }

        if (selected(selector, "TA-B1")) {
            assertFailure(callerUrl, "TA-B1-missing-send", "ta-b1", "NOT_FOUND", true);
            assertFailure(callerUrl, "TA-B1-missing-request", "ta-b1", "NOT_FOUND", false);
        }

        if (selected(selector, "TA-B2")) {
            Contracts.ActorRefWire first = ensureRef(actorUrl, "TA-B2-first", "actor-recreated");
            waitUntilReady(callerUrl, "TA-B2-first-ready", first.actorId());

            Contracts.DestroyActorRefRes firstDestroyed = ToActorHttpClient.postJson(
                actorUrl + "/destroy-ref",
                new Contracts.DestroyActorRefReq("TA-B2-first-destroy", first),
                Contracts.DestroyActorRefRes.class);
            require(firstDestroyed.destroyed() && firstDestroyed.errorKind() == null,
                "TA-B2 first exact destroy was not acknowledged: " + firstDestroyed.errorKind());
            waitUntilMissing(callerUrl, "TA-B2-first-missing", first.actorId());

            Contracts.ActorRefWire second = ensureRef(actorUrl, "TA-B2-second", first.actorId());
            require(first.actorId().equals(second.actorId()), "TA-B2 actor id changed during recreation");
            require(first.generation() != second.generation(), "TA-B2 actor generation was reused");
            waitUntilReady(callerUrl, "TA-B2-second-ready", second.actorId());
            assertCall(callerUrl, "TA-B2-send", second.actorId(), "recreated-send", "sent", true);
            assertCall(callerUrl, "TA-B2-request", second.actorId(), "recreated-request",
                "reply:recreated-request", false);

            Contracts.DestroyActorRefRes staleDestroy = ToActorHttpClient.postJson(
                actorUrl + "/destroy-ref",
                new Contracts.DestroyActorRefReq("TA-B2-stale-destroy", first),
                Contracts.DestroyActorRefRes.class);
            require(!staleDestroy.destroyed(), "TA-B2 stale ActorRef destroyed the new actor");
            require("INVALID_OPERATION".equals(staleDestroy.errorKind()),
                "TA-B2 stale ActorRef expected INVALID_OPERATION, got " + staleDestroy.errorKind());
            assertCall(callerUrl, "TA-B2-after-stale-destroy", second.actorId(), "still-current",
                "reply:still-current", false);
        }

        if (selected(selector, "TA-B3")) {
            Contracts.ActorRefWire routeDown = ensureRef(actorBUrl, "TA-B3-ready", "actor-route-down");
            waitUntilReady(callerUrl, "TA-B3-ready", routeDown.actorId());
            System.out.println("TA-B3-ready");
            waitUntilRouteDown(callerUrl, "TA-B3-route-down");
            assertFailure(callerUrl, "TA-B3-unavailable", routeDown.actorId(), "UNAVAILABLE", false);
            System.out.println("TA-B3-unavailable");
            waitUntilRouteReady(callerUrl, "TA-B3-route-recovered");
            assertCall(callerUrl, "TA-B3-recovered", routeDown.actorId(), "recovered",
                "reply:recovered", false);
        }

        assertActorEvidence(actorUrl, actorBUrl, selector);

        System.out.println("to-actor-messaging selector=" + selector + " result=passed");
        System.out.println("to-actor-messaging e2e result=passed");
    }

    private static ZLinkStreamConnector connectAndBind(
        String endpoint,
        Contracts.ActorRefWire actorRef) {
        ZLinkStreamConnector connector = ZLinkStreamConnectorFactory.create(new ZLinkStreamConnectorOptions(
            URI.create(endpoint),
            ZLinkStreamDispatchMode.IMMEDIATE,
            Duration.ofSeconds(10),
            2,
            Duration.ofSeconds(5),
            64 * 1024,
            64 * 1024,
            Integer.MAX_VALUE,
            true,
            Duration.ofSeconds(1),
            Duration.ofSeconds(5),
            false,
            Duration.ofMillis(250),
            Duration.ofSeconds(5),
            2.0,
            false,
            ZLinkStreamCompression.LZ4,
            ZLinkStreamPacketNameResolver.defaultResolver(),
            null));
        try {
            connector.connect().submit().toCompletableFuture().join();
            Contracts.BindActorRes reply = connector.request(new Contracts.BindActorReq(actorRef))
                .submit(Contracts.BindActorRes.class).toCompletableFuture().join();
            require(actorRef.actorId().equals(reply.actorId()), "bound actor id mismatch");
            require(actorRef.generation() == reply.generation(), "bound actor generation mismatch");
            return connector;
        } catch (RuntimeException error) {
            close(connector);
            throw error;
        }
    }

    private static void assertBoundPush(
        ZLinkStreamConnector connector,
        String actorUrl,
        String scenario,
        String actorId,
        String value) {
        var waiting = connector.waitFor(Contracts.BoundPushNotify.class)
            .timeout(Duration.ofSeconds(8))
            .submit(Contracts.BoundPushNotify.class);
        Contracts.BoundPushRes reply = ToActorHttpClient.postJson(
            actorUrl + "/push", new Contracts.BoundPushReq(scenario, actorId, value),
            Contracts.BoundPushRes.class);
        require(reply.submitted(), scenario + " push failed " + reply.errorKind());
        Contracts.BoundPushNotify notify = waiting.toCompletableFuture().join().payload();
        require(actorId.equals(notify.actorId()), scenario + " push actor mismatch");
        require(value.equals(notify.value()), scenario + " push value mismatch");
    }

    private static void assertBoundPushFailure(
        String actorUrl,
        String scenario,
        String actorId,
        String value,
        String expectedKind) {
        Contracts.BoundPushRes reply = ToActorHttpClient.postJson(
            actorUrl + "/push", new Contracts.BoundPushReq(scenario, actorId, value),
            Contracts.BoundPushRes.class);
        require(!reply.submitted(), scenario + " push unexpectedly succeeded");
        require(expectedKind.equals(reply.errorKind()),
            scenario + " expected " + expectedKind + " got " + reply.errorKind());
    }

    private static void assertSessionEvidence(
        String sessionUrl,
        String actorId,
        String kind,
        String message) {
        List<Contracts.ActorEvidence> evidence = sessionEvidence(sessionUrl);
        require(evidence.stream().anyMatch(item -> actorId.equals(item.actorId()) && kind.equals(item.kind())),
            message);
    }

    private static void assertNoSessionBinding(
        String sessionAUrl,
        String sessionBUrl,
        String actorId,
        String message) {
        boolean bound = Stream.concat(
                sessionEvidence(sessionAUrl).stream(), sessionEvidence(sessionBUrl).stream())
            .anyMatch(item -> actorId.equals(item.actorId()) && "actor-bound".equals(item.kind()));
        require(!bound, message);
    }

    private static List<Contracts.ActorEvidence> sessionEvidence(String sessionUrl) {
        return List.of(ToActorHttpClient.getJson(
            sessionUrl + "/evidence", Contracts.ActorEvidence[].class));
    }

    private static void close(ZLinkStreamConnector connector) {
        connector.close().submit().toCompletableFuture().join();
    }

    private static void ensure(String actorUrl, String scenario, String actorId) {
        ToActorHttpClient.postJson(
            actorUrl + "/ensure",
            new Contracts.ActorCallReq(scenario, actorId, "ensure"),
            Contracts.ActorCallRes.class);
    }

    private static Contracts.ActorRefWire ensureRef(String actorUrl, String scenario, String actorId) {
        return ToActorHttpClient.postJson(
            actorUrl + "/ensure-ref",
            new Contracts.ActorCallReq(scenario, actorId, "ensure"),
            Contracts.ActorRefWire.class);
    }

    private static void ensureReady(String actorUrl, String callerUrl, String scenario, String actorId) {
        ensure(actorUrl, scenario, actorId);
        waitUntilReady(callerUrl, scenario + "-ready", actorId);
    }

    private static void waitUntilReady(String callerUrl, String scenario, String actorId) {
        long deadline = System.nanoTime() + 5_000_000_000L;
        Contracts.ActorCallRes response = null;
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

    private static void waitUntilMissing(String callerUrl, String scenario, String actorId) {
        long deadline = System.nanoTime() + 8_000_000_000L;
        Contracts.ActorCallRes response = null;
        while (System.nanoTime() < deadline) {
            response = call(callerUrl, scenario, actorId, "after-destroy", false);
            if ("NOT_FOUND".equals(response.errorKind())) {
                return;
            }
            if (response.errorKind() != null && !isConvergenceError(response.errorKind())) {
                break;
            }
            sleepBriefly();
        }
        String result = response == null ? "no response" : response.errorKind() + "/" + response.result();
        throw new IllegalStateException(scenario + " actor remained reachable " + result);
    }

    private static void waitUntilRouteDown(String callerUrl, String scenario) {
        waitForRoute(callerUrl, scenario, false);
    }

    private static void waitUntilRouteReady(String callerUrl, String scenario) {
        waitForRoute(callerUrl, scenario, true);
    }

    private static void waitForRoute(String callerUrl, String scenario, boolean expectedReady) {
        long deadline = System.nanoTime() + 20_000_000_000L;
        Contracts.RouteStatusRes status = null;
        while (System.nanoTime() < deadline) {
            status = ToActorHttpClient.getJson(callerUrl + "/route-status", Contracts.RouteStatusRes.class);
            if (status.ready() == expectedReady) {
                return;
            }
            sleepBriefly();
        }
        throw new IllegalStateException(scenario + " expected ready=" + expectedReady
            + " got " + (status == null ? "no response" : status.ready()));
    }

    private static boolean isConvergenceError(String errorKind) {
        return "NOT_FOUND".equals(errorKind) || "UNAVAILABLE".equals(errorKind);
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
        Contracts.ActorCallRes response = call(callerUrl, scenario, actorId, value, send);
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
        Contracts.ActorCallRes response = ToActorHttpClient.postJson(
            callerUrl + endpoint,
            new Contracts.ActorCallReq(scenario, actorId, "missing"),
            Contracts.ActorCallRes.class);
        require(expectedKind.equals(response.errorKind()),
            scenario + " expected " + expectedKind + " got " + response.errorKind());
    }

    private static Contracts.ActorCallRes call(
        String callerUrl,
        String scenario,
        String actorId,
        String value,
        boolean send) {
        String endpoint = send ? "/send" : "/request";
        return ToActorHttpClient.postJson(
            callerUrl + endpoint,
            new Contracts.ActorCallReq(scenario, actorId, value),
            Contracts.ActorCallRes.class);
    }

    private static void assertActorEvidence(String actorUrl, String actorBUrl, String selector) {
        List<Contracts.ActorEvidence> evidence = new ArrayList<>(List.of(
            ToActorHttpClient.getJson(actorUrl + "/evidence", Contracts.ActorEvidence[].class)));
        if (selected(selector, "TA-B3")) {
            evidence.addAll(List.of(
                ToActorHttpClient.getJson(actorBUrl + "/evidence", Contracts.ActorEvidence[].class)));
        }
        if (selected(selector, "TA-A1")) {
            require(containsEvidence(evidence, "TA-A1-send", "ta-a1", "send"), "TA-A1 send evidence missing");
            require(containsEvidence(evidence, "TA-A1-request", "ta-a1", "request"), "TA-A1 request evidence missing");
            require(containsEvidence(evidence, "TA-A1-before", "ta-a1", "bound-push"),
                "TA-A1 Before push evidence missing");
            require(containsEvidence(evidence, "TA-A1-after", "ta-a1", "bound-push"),
                "TA-A1 After push evidence missing");
        }
        if (selected(selector, "TA-A2")) {
            require(containsEvidence(evidence, "TA-A2-send", "ta-a2", "send"), "TA-A2 send evidence missing");
            require(containsEvidence(evidence, "TA-A2-request", "ta-a2", "request"),
                "TA-A2 request evidence missing");
            require(!containsScenario(evidence, "TA-A2-push"), "TA-A2 unbound push reached the handler");
        }
        if (selected(selector, "TA-A3")) {
            require(containsEvidence(evidence, "TA-A3-before-bind-send", "ta-a3", "send"),
                "TA-A3 pre-bind send evidence missing");
            require(containsEvidence(evidence, "TA-A3-before-bind-request", "ta-a3", "request"),
                "TA-A3 pre-bind request evidence missing");
            require(containsEvidence(evidence, "TA-A3-after-bind-request", "ta-a3", "request"),
                "TA-A3 request evidence missing");
            require(containsEvidence(evidence, "TA-A3-after-bind-push", "ta-a3", "bound-push"),
                "TA-A3 late-bind push evidence missing");
            require(!containsScenario(evidence, "TA-A3-before-bind-push"),
                "TA-A3 pre-bind push reached the handler");
        }
        if (selected(selector, "TA-A4")) {
            require(containsEvidence(evidence, "TA-A4-disconnected-send", "ta-a4", "send"),
                "TA-A4 send evidence missing");
            require(containsEvidence(evidence, "TA-A4-disconnected-request", "ta-a4", "request"),
                "TA-A4 request evidence missing");
            require(containsEvidence(evidence, "TA-A4-before-disconnect", "ta-a4", "bound-push"),
                "TA-A4 pre-unbind push evidence missing");
            require(containsEvidence(evidence, "TA-A4-unbind", "ta-a4", "session-unbound"),
                "TA-A4 unbind evidence missing");
            require(containsEvidence(evidence, "TA-A4-destroy", "ta-a4", "destroy"),
                "TA-A4 destroy evidence missing");
            require(!containsScenario(evidence, "TA-A4-after-disconnect-push"),
                "TA-A4 post-unbind push reached the handler");
            require(!containsScenario(evidence, "TA-A4-destroyed"),
                "TA-A4 request reached the actor after destroy");
        }
        if (selected(selector, "TA-B1")) {
            require(!containsScenario(evidence, "TA-B1-missing-send"), "TA-B1 missing actor send reached actor");
            require(!containsScenario(evidence, "TA-B1-missing-request"), "TA-B1 missing actor request reached actor");
        }
        if (selected(selector, "TA-B2")) {
            require(containsEvidence(evidence, "TA-B2-send", "actor-recreated", "send"),
                "TA-B2 recreated send evidence missing");
            require(containsEvidence(evidence, "TA-B2-request", "actor-recreated", "request"),
                "TA-B2 recreated request evidence missing");
            require(containsEvidence(evidence, "TA-B2-after-stale-destroy", "actor-recreated", "request"),
                "TA-B2 post-stale-destroy request evidence missing");
            require(!containsScenario(evidence, "TA-B2-stale-destroy"),
                "TA-B2 stale destroy reached an actor handler");
        }
        if (selected(selector, "TA-B3")) {
            require(containsEvidence(evidence, "TA-B3-recovered", "actor-route-down", "request"),
                "TA-B3 recovered request evidence missing");
            require(!containsScenario(evidence, "TA-B3-unavailable"),
                "TA-B3 unavailable request reached an actor handler");
        }
    }

    private static boolean selected(String selector, String scenario) {
        return "all".equals(selector) || scenario.equals(selector);
    }

    private static boolean containsEvidence(
        List<Contracts.ActorEvidence> evidence,
        String scenario,
        String actorId,
        String kind) {
        return evidence.stream().anyMatch(item ->
            scenario.equals(item.scenario()) && actorId.equals(item.actorId()) && kind.equals(item.kind()));
    }

    private static boolean containsScenario(List<Contracts.ActorEvidence> evidence, String scenario) {
        return evidence.stream().anyMatch(item -> scenario.equals(item.scenario()));
    }

    private static void require(boolean condition, String message) {
        if (!condition) {
            throw new IllegalStateException(message);
        }
    }

    private static IllegalStateException contractBlocker(String message) {
        return new IllegalStateException("contract blocker: " + message);
    }
}
