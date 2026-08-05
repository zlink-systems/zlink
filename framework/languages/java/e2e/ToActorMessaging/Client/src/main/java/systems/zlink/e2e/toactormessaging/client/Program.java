package systems.zlink.e2e.toactormessaging.client;

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
        String callerUrl = options.callerHttpEndpoint();
        String sessionAUrl = options.sessionAHttpEndpoint();
        String sessionBUrl = options.sessionBHttpEndpoint();
        String sessionAStream = options.sessionAStreamEndpoint();
        String sessionBStream = options.sessionBStreamEndpoint();
        String selector = args[3];
        require(java.util.Set.of("all", "TA-A1", "TA-A2", "TA-A3", "TA-A4",
            "TA-B1", "TA-B2", "TA-B3").contains(selector),
            "unknown ToActorMessaging selector: " + selector);

        if (selected(selector, "TA-A1")) {
            Contracts.ActorRefWire actorRef = ensureRef(actorUrl, "TA-A1", "ta-a1");
            waitRefUntilReady(callerUrl, "TA-A1-ready", actorRef);
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
            assertBoundPushFailure(actorUrl, "TA-A2-push", "ta-a2", "Unbound", "REQUEST_FAILED");
            assertNoSessionBinding(sessionAUrl, sessionBUrl, "ta-a2", "TA-A2 actor was unexpectedly bound");
        }

        if (selected(selector, "TA-A3")) {
            ensureReady(actorUrl, callerUrl, "TA-A3", "ta-a3");
            assertCall(callerUrl, "TA-A3-before-bind-send", "ta-a3", "a3-before-send", "sent", true);
            assertCall(callerUrl, "TA-A3-before-bind-request", "ta-a3", "a3-before-request",
                "reply:a3-before-request", false);
            assertBoundPushFailure(actorUrl, "TA-A3-before-bind-push", "ta-a3", "BeforeBind",
                "REQUEST_FAILED");
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
            waitRefUntilReady(callerUrl, "TA-A4-ready", actorRef);
            ZLinkStreamConnector connector = connectAndBind(sessionAStream, actorRef);
            assertBoundPush(connector, actorUrl, "TA-A4-before-disconnect", "ta-a4", "BeforeDisconnect");
            Contracts.UnbindActorReply unbound = ToActorHttpClient.postJson(
                actorUrl + "/unbind", new Contracts.UnbindActorRequest("TA-A4-unbind", "ta-a4"),
                Contracts.UnbindActorReply.class);
            require(unbound.unbound(), "TA-A4 unbind was not acknowledged");
            assertSessionEvidence(sessionAUrl, "ta-a4", "actor-bound", "TA-A4 bind evidence missing");
            assertBoundPushFailure(actorUrl, "TA-A4-after-disconnect-push", "ta-a4", "AfterDisconnect",
                "REQUEST_FAILED");
            assertCall(callerUrl, "TA-A4-disconnected-send", "ta-a4", "a4-send", "sent", true);
            assertCall(callerUrl, "TA-A4-disconnected-request", "ta-a4", "a4-request", "reply:a4-request", false);
            close(connector);
            Contracts.DestroyActorReply destroyed = ToActorHttpClient.postJson(
                actorUrl + "/destroy", new Contracts.DestroyActorRequest("TA-A4-destroy", "ta-a4"),
                Contracts.DestroyActorReply.class);
            require(destroyed.destroyed(), "TA-A4 destroy was not acknowledged");
            waitUntilMissing(callerUrl, "TA-A4-destroyed", "ta-a4");
        }

        if (selected(selector, "TA-B1")) {
            Contracts.ActorRefWire missingRef = ensureRef(actorUrl, "TA-B1", "ta-b1");
            waitRefUntilReady(callerUrl, "TA-B1-ref-ready", missingRef);
            Contracts.DestroyActorReply destroyed = ToActorHttpClient.postJson(
                actorUrl + "/destroy",
                new Contracts.DestroyActorRequest("TA-B1-destroy", missingRef.actorId()),
                Contracts.DestroyActorReply.class);
            require(destroyed.destroyed(), "TA-B1 destroy was not acknowledged");
            waitUntilMissing(callerUrl, "TA-B1-row-removed", missingRef.actorId());
            Contracts.ActorCallResponse missingSend = refCall(
                callerUrl, "TA-B1-missing-send", missingRef, "missing", true);
            require(missingSend.errorKind() == null && "sent".equals(missingSend.result()),
                "TA-B1 one-way send submit was not accepted locally");
            assertRefFailure(callerUrl, "TA-B1-missing-request", missingRef, "ACTOR_ROUTE_NOT_FOUND", false);
        }

        if (selected(selector, "TA-B2")) {
            Contracts.ActorRefWire b2Ref = ensureRef(actorUrl, "TA-B2", "ta-b2");
            waitRefUntilReady(callerUrl, "TA-B2-ref-ready", b2Ref);
            Contracts.ActorRefWire staleB2Ref = new Contracts.ActorRefWire(
                b2Ref.nodeRidHex(), b2Ref.actorId(), b2Ref.generation() + 1);
            assertRefFailure(callerUrl, "TA-B2-stale-ref", staleB2Ref, "ACTOR_LOCATION_STALE", false);
            assertCall(callerUrl, "TA-B2-live-after-reresolve", "ta-b2", "b2-request", "reply:b2-request", false);
        }

        if (selected(selector, "TA-B3")) {
            Contracts.ActorRefWire b3Ref = ensureRef(actorUrl, "TA-B3", "ta-b3");
            waitRefUntilReady(callerUrl, "TA-B3-ref-ready", b3Ref);
            Contracts.ActorRefWire disconnectedB3Ref = new Contracts.ActorRefWire(
                systems.zlink.contracts.core.RoutingId.from("actor-missing-route").toHex(),
                b3Ref.actorId(), b3Ref.generation());
            assertRefFailure(callerUrl, "TA-B3-route-disconnected", disconnectedB3Ref, "ROUTE_NOT_CONNECTED", false);
            assertCall(callerUrl, "TA-B3-route-restored", "ta-b3", "b3-request", "reply:b3-request", false);
        }

        assertActorEvidence(actorUrl, selector);

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
            Contracts.BindActorReply reply = connector.request(new Contracts.BindActorRequest(actorRef))
                .submit(Contracts.BindActorReply.class).toCompletableFuture().join();
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
        Contracts.BoundPushReply reply = ToActorHttpClient.postJson(
            actorUrl + "/push", new Contracts.BoundPushRequest(scenario, actorId, value),
            Contracts.BoundPushReply.class);
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
        Contracts.BoundPushReply reply = ToActorHttpClient.postJson(
            actorUrl + "/push", new Contracts.BoundPushRequest(scenario, actorId, value),
            Contracts.BoundPushReply.class);
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
        boolean bound = java.util.stream.Stream.concat(
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
            new Contracts.ActorCallRequest(scenario, actorId, "ensure"),
            Contracts.ActorCallResponse.class);
    }

    private static Contracts.ActorRefWire ensureRef(String actorUrl, String scenario, String actorId) {
        return ToActorHttpClient.postJson(
            actorUrl + "/ensure-ref",
            new Contracts.ActorCallRequest(scenario, actorId, "ensure"),
            Contracts.ActorRefWire.class);
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

    private static void waitUntilMissing(String callerUrl, String scenario, String actorId) {
        long deadline = System.nanoTime() + 8_000_000_000L;
        Contracts.ActorCallResponse response = null;
        while (System.nanoTime() < deadline) {
            response = call(callerUrl, scenario, actorId, "after-destroy", false);
            if ("ACTOR_ROUTE_NOT_FOUND".equals(response.errorKind())) {
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

    private static void waitRefUntilReady(
        String callerUrl,
        String scenario,
        Contracts.ActorRefWire actorRef) {
        long deadline = System.nanoTime() + 30_000_000_000L;
        Contracts.ActorCallResponse response = null;
        while (System.nanoTime() < deadline) {
            response = refCall(callerUrl, scenario, actorRef, "ready", false);
            if (response.errorKind() == null && "reply:ready".equals(response.result())) {
                return;
            }
            if (!isConvergenceError(response.errorKind()) && !"ROUTE_NOT_CONNECTED".equals(response.errorKind())) {
                break;
            }
            sleepBriefly();
        }
        String error = response == null ? "no response" : response.errorKind();
        throw new IllegalStateException(scenario + " ref readiness failed " + error);
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
        Contracts.ActorCallResponse response = ToActorHttpClient.postJson(
            callerUrl + endpoint,
            new Contracts.ActorCallRequest(scenario, actorId, "missing"),
            Contracts.ActorCallResponse.class);
        require(expectedKind.equals(response.errorKind()),
            scenario + " expected " + expectedKind + " got " + response.errorKind());
    }

    private static void assertRefFailure(
        String callerUrl,
        String scenario,
        Contracts.ActorRefWire actorRef,
        String expectedKind,
        boolean send) {
        String endpoint = send ? "/send-ref" : "/request-ref";
        Contracts.ActorCallResponse response = ToActorHttpClient.postJson(
            callerUrl + endpoint,
            new Contracts.ActorRefCallRequest(scenario, actorRef, "fault"),
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
        return ToActorHttpClient.postJson(
            callerUrl + endpoint,
            new Contracts.ActorCallRequest(scenario, actorId, value),
            Contracts.ActorCallResponse.class);
    }

    private static Contracts.ActorCallResponse refCall(
        String callerUrl,
        String scenario,
        Contracts.ActorRefWire actorRef,
        String value,
        boolean send) {
        String endpoint = send ? "/send-ref" : "/request-ref";
        return ToActorHttpClient.postJson(
            callerUrl + endpoint,
            new Contracts.ActorRefCallRequest(scenario, actorRef, value),
            Contracts.ActorCallResponse.class);
    }

    private static void assertActorEvidence(String actorUrl, String selector) {
        List<Contracts.ActorEvidence> evidence = List.of(
            ToActorHttpClient.getJson(actorUrl + "/evidence", Contracts.ActorEvidence[].class));
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
            require(containsEvidence(evidence, "TA-B2-live-after-reresolve", "ta-b2", "request"),
                "TA-B2 live follow-up evidence missing");
            require(!containsScenario(evidence, "TA-B2-stale-ref"), "TA-B2 stale ref reached actor");
        }
        if (selected(selector, "TA-B3")) {
            require(containsEvidence(evidence, "TA-B3-route-restored", "ta-b3", "request"),
                "TA-B3 restored route evidence missing");
            require(!containsScenario(evidence, "TA-B3-route-disconnected"),
                "TA-B3 disconnected route reached actor");
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
}
