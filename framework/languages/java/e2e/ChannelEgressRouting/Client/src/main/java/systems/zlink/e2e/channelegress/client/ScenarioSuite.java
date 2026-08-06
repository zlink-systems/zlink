package systems.zlink.e2e.channelegress.client;

import java.net.URI;
import java.time.Duration;
import java.util.HashSet;
import java.util.List;
import java.util.Map;
import java.util.Set;
import java.util.UUID;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.TimeUnit;
import systems.zlink.stream.connector.ZLinkStreamConnector;
import systems.zlink.stream.connector.ZLinkStreamConnectorFactory;
import systems.zlink.stream.connector.ZLinkStreamConnectorOptions;
import systems.zlink.stream.connector.ZLinkStreamEncodedPayload;
import systems.zlink.contracts.messaging.Message;
import systems.zlink.e2e.channelegress.shared.Contracts;

public final class ScenarioSuite {
    private static final List<String> ALL = List.of(
        "CH-E2E-01", "CH-E2E-02", "CH-E2E-03",
        "CH-E2E-04A", "CH-E2E-04B", "CH-E2E-04C",
        "CH-E2E-05", "CH-E2E-06", "CH-E2E-07A", "CH-E2E-07B", "CH-E2E-07C",
        "CH-E2E-08", "CH-E2E-09", "CH-E2E-10", "CH-E2E-11", "CH-E2E-12");

    private ScenarioSuite() {
    }

    public static void run(String selector, ClientOptions options) {
        List<String> selected = "all".equalsIgnoreCase(selector)
            ? ALL
            : List.of(selector.split(","));
        for (String raw : selected) {
            String scenario = raw.trim().toUpperCase();
            switch (scenario) {
                case "CH-E2E-01" -> ch01(options);
                case "CH-E2E-02" -> ch02(options);
                case "CH-E2E-03" -> ch03(options);
                case "CH-E2E-04A" -> ch04a(options);
                case "CH-E2E-04B" -> ch04b(options);
                case "CH-E2E-04C" -> ch04c(options);
                case "CH-E2E-05" -> ch05(options);
                case "CH-E2E-06" -> ch06(options);
                case "CH-E2E-07A" -> ch07a(options);
                case "CH-E2E-07B" -> ch07b(options);
                case "CH-E2E-07C" -> ch07c(options);
                case "CH-E2E-08" -> ch08(options);
                case "CH-E2E-09" -> ch09(options);
                case "CH-E2E-10" -> ch10(options);
                case "CH-E2E-11" -> ch11(options);
                case "CH-E2E-12" -> ch12(options);
                default -> throw new IllegalArgumentException(
                    "ChannelEgressRouting scenario is not implemented: " + scenario);
            }
            System.out.println("scenario " + scenario + " passed");
        }
    }

    private static void ch01(ClientOptions options) {
        String operation = id("ch-01");
        Contracts.InvokeRes forward = request(
            options.sessionEndpoint(), Contracts.PLAY_CHANNEL, operation, "echo");
        Contracts.InvokeRes reverse = request(
            options.playEndpoint(), Contracts.SESSION_CHANNEL, operation + "-reverse", "echo");
        succeeded(forward, "CH-E2E-01 forward");
        succeeded(reverse, "CH-E2E-01 reverse");
        require("play".equals(forward.reply().role()), "forward request did not reach Play");
        require("session".equals(reverse.reply().role()), "reverse request did not reach Session");
        waitFor(options.playEndpoint(), operation);
        waitFor(options.sessionEndpoint(), operation + "-reverse");
    }

    private static void ch02(ClientOptions options) {
        String operation = id("ch-02");
        Contracts.InvokeRes result = request(
            options.sessionEndpoint(), Contracts.PLAY_CHANNEL, operation, "cascade");
        succeeded(result, "CH-E2E-02 cascade");
        require(result.reply().downstream().size() == 2,
            "cascade did not return both downstream replies");
        waitFor(options.auditEndpoint(), operation + "-audit");
        waitForAny(options.workflowAEndpoint(), options.workflowBEndpoint(), operation + "-workflow");
    }

    private static void ch03(ClientOptions options) {
        String operation = id("ch-03");
        String spotId = operation + "-spot";
        Contracts.SpotCreateRes created = ClientHttp.post(
            options.playEndpoint(), "/objects/spots",
            new Contracts.SpotCreateReq(spotId), Contracts.SpotCreateRes.class);
        require(spotId.equals(created.spotId()), "CH-E2E-03 created a different Spot");
        Contracts.SpotWorkflowRes reply = ClientHttp.post(
            options.playEndpoint(), "/objects/spots/" + spotId + "/workflow",
            new Contracts.SpotWorkflowReq(operation), Contracts.SpotWorkflowRes.class);
        require(reply.sequence().equals(List.of(
            "handler-start", "workflow-reply", "handler-end", "timer-start")),
            "CH-E2E-03 handler sequence was " + reply.sequence());
        waitFor(options.playEndpoint(),
            "sequence=handler-start,workflow-reply,handler-end,timer-start,workflow-reply,timer-end");
    }

    private static void ch04a(ClientOptions options) {
        WeightedResult result = weighted(
            options.callerEndpoint(), options, "ch-04a", 800, Set.of("workflow-a", "workflow-b"));
        double ratio = result.second() / 800.0;
        require(ratio >= 0.65 && ratio <= 0.85,
            "CH-E2E-04A weight-300 ratio was " + ratio + " counts=" + result);
    }

    private static void ch04b(ClientOptions options) {
        postControl(options.workflowBEndpoint(), "/control/weight/0");
        waitWorkflowTargets(options.callerEndpoint(), 1);
        postControl(options.workflowAEndpoint(), "/control/hold");
        String heldId = id("ch-04b-held");
        CompletableFuture<Contracts.InvokeRes> held = CompletableFuture.supplyAsync(() ->
            request(options.callerEndpoint(), Contracts.WORKFLOW_CHANNEL, heldId, "hold"));
        waitFor(options.workflowAEndpoint(), heldId);

        postControl(options.workflowBEndpoint(), "/control/weight/100");
        postControl(options.workflowAEndpoint(), "/shutdown");
        Contracts.InvokeRes converged = requestEventuallyExpectedRole(
            options.callerEndpoint(), "workflow-b", "ch-04b-new-0");
        succeeded(converged, "CH-E2E-04B first post-drain request");
        for (int index = 1; index < 50; index++) {
            Contracts.InvokeRes next = request(
                options.callerEndpoint(), Contracts.WORKFLOW_CHANNEL,
                id("ch-04b-new-" + index), "echo");
            succeeded(next, "CH-E2E-04B new request");
            require("workflow-b".equals(next.reply().role()),
                "drained server accepted a new request: " + next.reply().role());
        }
        postControl(options.workflowAEndpoint(), "/control/release");
        Contracts.InvokeRes first;
        try {
            first = held.get(10, TimeUnit.SECONDS);
        } catch (Exception error) {
            throw new IllegalStateException("held request did not complete", error);
        }
        succeeded(first, "CH-E2E-04B held request");
        require("workflow-a".equals(first.reply().role()),
            "held request changed target after drain");
    }

    private static void ch04c(ClientOptions options) {
        Contracts.InvokeRes result = request(
            options.callerEndpoint(), Contracts.WORKFLOW_CHANNEL, id("ch-04c"), "echo");
        succeeded(result, "CH-E2E-04C replacement request");
        require("workflow-new".equals(result.reply().lifecycle()),
            "request used stale lifecycle " + result.reply().lifecycle());
    }

    private static void ch05(ClientOptions options) {
        String rejectedId = id("ch-05-server-only");
        Contracts.InvokeRes rejected = request(
            options.workflowBEndpoint(), Contracts.WORKFLOW_CHANNEL, rejectedId, "echo");
        error(rejected, "NOT_FOUND", "CH-E2E-05 server-only request");
        require(count(options.workflowBEndpoint(), rejectedId) == 0,
            "server-only request ran its local handler");
        Contracts.InvokeRes normal = request(
            options.callerEndpoint(), Contracts.WORKFLOW_CHANNEL, id("ch-05-normal"), "echo");
        succeeded(normal, "CH-E2E-05 normal Client request");
    }

    private static void ch06(ClientOptions options) {
        Contracts.InvokeRes route = request(
            options.sessionEndpoint(), Contracts.PLAY_CHANNEL, id("ch-06-route"), "echo");
        Contracts.InvokeRes workflow = request(
            options.callerEndpoint(), Contracts.WORKFLOW_CHANNEL, id("ch-06-workflow"), "echo");
        succeeded(route, "CH-E2E-06 distinct RouteMesh channel");
        succeeded(workflow, "CH-E2E-06 distinct ClientServer channel");
    }

    private static void ch07a(ClientOptions options) {
        String operation = id("ch-07a");
        Contracts.InvokeRes result = request(
            options.sessionEndpoint(), "missing.channel", operation, "echo");
        error(result, "NOT_FOUND", "CH-E2E-07A missing channel");
        require(result.elapsedMilliseconds() < 1_000,
            "missing channel did not fail immediately");
        require(count(options.sessionEndpoint(), operation) == 0,
            "missing channel reached an application handler");
    }

    private static void ch07b(ClientOptions options) {
        int remote = 0;
        Set<String> operations = new HashSet<>();
        for (int index = 0; index < 20; index++) {
            String operation = id("ch-07b-" + index);
            operations.add(operation);
            Contracts.InvokeRes result = request(
                options.apiAEndpoint(), Contracts.API_CHANNEL, operation, "echo");
            succeeded(result, "CH-E2E-07B server-role request");
            if ("api-b".equals(result.reply().role())) {
                remote++;
            }
        }
        require(remote > 0, "server-role requests never selected the remote member");
        require(totalRequestCount(List.of(options.apiAEndpoint(), options.apiBEndpoint()), "ch-07b-") == 20,
            "CH-E2E-07B handler count did not equal 20");
        require(operations.size() == 20, "CH-E2E-07B operation ids were not unique");
    }

    private static void ch07c(ClientOptions options) {
        String operation = id("ch-07c");
        Contracts.InvokeRes result = request(
            options.sessionEndpoint(), Contracts.API_CHANNEL, operation, "echo");
        error(result, "UNAVAILABLE", "CH-E2E-07C unavailable target");
        require(count(options.sessionEndpoint(), operation) == 0,
            "unavailable request reached another handler");
    }

    private static void ch08(ClientOptions options) {
        String operation = id("ch-08");
        String spotId = operation + "-spot";
        String actorId = operation + "-actor";
        ClientHttp.post(options.playEndpoint(), "/objects/spots",
            new Contracts.SpotCreateReq(spotId), Contracts.SpotCreateRes.class);
        ClientHttp.post(options.playEndpoint(), "/objects/actors",
            new Contracts.ActorCreateReq(actorId), Contracts.ActorCreateRes.class);
        Contracts.StateAddressRes result = ClientHttp.post(
            options.callerEndpoint(), "/objects/state-address",
            new Contracts.StateAddressReq(operation, spotId, actorId),
            Contracts.StateAddressRes.class);
        require(result.downstream().size() == 2,
            "state-address handler did not return Spot and Actor replies");
        require(result.downstream().get(0).startsWith("spot:" + spotId + ":"),
            "Spot result was not first");
        require(result.downstream().get(1).startsWith("actor:" + actorId + ":"),
            "Actor result was not second");
        waitFor(options.playEndpoint(), "spot-request");
        waitFor(options.playEndpoint(), "actor-request");
    }

    private static void ch09(ClientOptions options) {
        List<Contracts.ListenerStatus> listenerRows = new java.util.ArrayList<>();
        listenerRows.addAll(List.of(ClientHttp.get(
            options.apiAEndpoint(), "/status/listeners", Contracts.ListenerStatus[].class)));
        listenerRows.addAll(List.of(ClientHttp.get(
            options.workflowAEndpoint(), "/status/listeners", Contracts.ListenerStatus[].class)));
        Set<String> expected = Set.of("RouteMesh", "ClientServer", "Fanout", "STREAM");
        Set<String> observed = new HashSet<>();
        for (Contracts.ListenerStatus row : listenerRows) {
            if (!expected.contains(row.kind())) {
                continue;
            }
            observed.add(row.kind());
            require(row.isReady(), row.kind() + " listener was not ready");
            require(row.advertisedEndpoint() != null
                    && row.advertisedEndpoint().startsWith("tcp://127.0.0.1:"),
                row.kind() + " advertised endpoint did not use AdvertiseHost: "
                    + row.advertisedEndpoint());
            require(!row.advertisedEndpoint().endsWith(":0")
                    && !row.advertisedEndpoint().contains("0.0.0.0"),
                row.kind() + " advertised endpoint retained wildcard or port 0: "
                    + row.advertisedEndpoint());
        }
        require(observed.containsAll(expected),
            "CH-E2E-09 listener status omitted " + difference(expected, observed));
        succeeded(request(options.sessionEndpoint(), Contracts.API_CHANNEL,
            id("ch-09-route"), "echo"), "CH-E2E-09 RouteMesh port 0 request");
        succeeded(request(options.callerEndpoint(), Contracts.WORKFLOW_CHANNEL,
            id("ch-09-workflow"), "echo"), "CH-E2E-09 ClientServer port 0 request");
        String fanoutId = id("ch-09-fanout");
        ClientHttp.post(options.apiAEndpoint(), "/fanout/publish",
            new Contracts.FanoutProbe(fanoutId), Map.class);
        waitFor(options.fanoutSubscriberEndpoint(), fanoutId);

        String streamEndpoint = listenerRows.stream()
            .filter(row -> "STREAM".equals(row.kind()))
            .findFirst()
            .orElseThrow(() -> new IllegalStateException("STREAM listener status is missing"))
            .advertisedEndpoint();
        String streamId = id("ch-09-stream");
        ZLinkStreamConnector connector = ZLinkStreamConnectorFactory.create(
            ZLinkStreamConnectorOptions.createDefault(URI.create(streamEndpoint)));
        try {
            connector.connect().submit().toCompletableFuture().join();
            connector.send(new ZLinkStreamEncodedPayload(
                    "StreamProbe", Message.from(streamId), Map.of()))
                .submit()
                .toCompletableFuture()
                .join();
        } finally {
            connector.close().submit().toCompletableFuture().join();
        }
        waitFor(options.apiAEndpoint(), "packet=StreamProbe");
    }

    private static Set<String> difference(Set<String> expected, Set<String> observed) {
        Set<String> missing = new HashSet<>(expected);
        missing.removeAll(observed);
        return missing;
    }

    private static void ch10(ClientOptions options) {
        String operation = id("ch-10");
        Contracts.SendRes result = ClientHttp.post(
            options.callerEndpoint(), "/send",
            new Contracts.InvokeReq(Contracts.WORKFLOW_CHANNEL, operation),
            Contracts.SendRes.class);
        require(result.succeeded(), "CH-E2E-10 send failed: " + result.error());
        waitForAny(options.workflowAEndpoint(), options.workflowBEndpoint(), operation);
        int handled = count(options.workflowAEndpoint(), operation)
            + count(options.workflowBEndpoint(), operation);
        require(handled == 1, "one-way send was handled " + handled + " times");
    }

    private static void ch11(ClientOptions options) {
        String operation = id("ch-11");
        Contracts.InvokeRes request = request(
            options.sessionEndpoint(), Contracts.API_CHANNEL, operation, "echo");
        Contracts.SendRes send = ClientHttp.post(
            options.sessionEndpoint(), "/send",
            new Contracts.InvokeReq(Contracts.API_CHANNEL, operation + "-send"),
            Contracts.SendRes.class);
        succeeded(request, "CH-E2E-11 request");
        require(send.succeeded(), "CH-E2E-11 send failed: " + send.error());
        waitForAny(options.apiAEndpoint(), options.apiBEndpoint(), operation);
        waitForAny(options.apiAEndpoint(), options.apiBEndpoint(), operation + "-send");
    }

    private static void ch12(ClientOptions options) {
        WeightedResult result = weighted(
            options.workflowAEndpoint(), options, "ch-12", 400,
            Set.of("workflow-a", "workflow-b"));
        double localRatio = result.first() / 400.0;
        require(localRatio >= 0.35 && localRatio <= 0.65,
            "CH-E2E-12 local ratio was " + localRatio + " counts=" + result);
    }

    private static WeightedResult weighted(
        String source,
        ClientOptions options,
        String prefix,
        int requestCount,
        Set<String> expectedRoles) {
        int first = 0;
        int second = 0;
        Set<String> replies = new HashSet<>();
        for (int index = 0; index < requestCount; index++) {
            Contracts.InvokeRes result = request(
                source, Contracts.WORKFLOW_CHANNEL, prefix + "-" + index, "echo");
            succeeded(result, prefix + " request " + index);
            require(expectedRoles.contains(result.reply().role()),
                "unexpected workflow role " + result.reply().role());
            require(replies.add(result.reply().id()),
                "duplicate workflow reply id " + result.reply().id());
            if ("workflow-a".equals(result.reply().role())) {
                first++;
            } else {
                second++;
            }
        }
        require(first + second == requestCount, "workflow reply count mismatch");
        int evidence = requestCount(options.workflowAEndpoint(), prefix)
            + requestCount(options.workflowBEndpoint(), prefix);
        require(evidence == requestCount,
            "workflow handler count mismatch: " + evidence + " != " + requestCount);
        return new WeightedResult(first, second);
    }

    private static Contracts.InvokeRes request(
        String endpoint,
        String channel,
        String operation,
        String mode) {
        return ClientHttp.post(endpoint, "/request",
            new Contracts.InvokeReq(channel, operation, mode), Contracts.InvokeRes.class);
    }

    private static void postControl(String endpoint, String path) {
        ClientHttp.post(endpoint, path, Map.of(), Map.class);
    }

    private static void waitWorkflowTargets(String endpoint, int expected) {
        long deadline = System.nanoTime() + Duration.ofSeconds(20).toNanos();
        Contracts.WorkflowStatus last = null;
        while (System.nanoTime() < deadline) {
            last = ClientHttp.get(endpoint, "/status/workflow", Contracts.WorkflowStatus.class);
            if (last.readyTargetCount() == expected) {
                return;
            }
            pause();
        }
        throw new IllegalStateException(
            "workflow target count did not become " + expected + ": " + last);
    }

    private static Contracts.InvokeRes requestEventuallyExpectedRole(
        String endpoint,
        String expectedRole,
        String prefix) {
        long deadline = System.nanoTime() + Duration.ofSeconds(20).toNanos();
        Contracts.InvokeRes last = null;
        int attempt = 0;
        while (System.nanoTime() < deadline) {
            last = request(
                endpoint, Contracts.WORKFLOW_CHANNEL, id(prefix + "-probe-" + attempt++), "echo");
            if (last.succeeded()) {
                require(expectedRole.equals(last.reply().role()),
                    "drained server accepted a new request: " + last.reply().role());
                return last;
            }
            pause();
        }
        throw new IllegalStateException(
            "workflow did not converge to " + expectedRole + ": " + last);
    }

    private static void succeeded(Contracts.InvokeRes result, String label) {
        require(result.succeeded(), label + " failed: " + result.error());
        require(result.reply() != null, label + " returned no reply");
    }

    private static void error(Contracts.InvokeRes result, String expected, String label) {
        require(!result.succeeded(), label + " unexpectedly succeeded");
        require(expected.equalsIgnoreCase(result.error()),
            label + " returned " + result.error() + ", expected " + expected);
    }

    private static void waitFor(String endpoint, String fragment) {
        long deadline = System.nanoTime() + Duration.ofSeconds(20).toNanos();
        while (System.nanoTime() < deadline) {
            if (count(endpoint, fragment) > 0) {
                return;
            }
            pause();
        }
        throw new IllegalStateException("timed out waiting for " + fragment + " at " + endpoint);
    }

    private static void waitForAny(String first, String second, String fragment) {
        long deadline = System.nanoTime() + Duration.ofSeconds(20).toNanos();
        while (System.nanoTime() < deadline) {
            if (count(first, fragment) + count(second, fragment) > 0) {
                return;
            }
            pause();
        }
        throw new IllegalStateException("timed out waiting for " + fragment);
    }

    private static int totalRequestCount(List<String> endpoints, String fragment) {
        return endpoints.stream().mapToInt(endpoint -> requestCount(endpoint, fragment)).sum();
    }

    private static int requestCount(String endpoint, String fragment) {
        Contracts.EvidenceSnapshot snapshot = ClientHttp.get(
            endpoint, "/evidence", Contracts.EvidenceSnapshot.class);
        return (int) snapshot.entries().stream()
            .filter(entry -> "request-start".equals(entry.marker()))
            .map(ScenarioSuite::evidenceLine)
            .filter(line -> line.contains(fragment))
            .count();
    }

    private static int count(String endpoint, String fragment) {
        Contracts.EvidenceSnapshot snapshot = ClientHttp.get(
            endpoint, "/evidence", Contracts.EvidenceSnapshot.class);
        return (int) snapshot.entries().stream()
            .map(ScenarioSuite::evidenceLine)
            .filter(line -> line.contains(fragment))
            .count();
    }

    private static String evidenceLine(Contracts.EvidenceEntry entry) {
        return entry.marker() + "|role=" + entry.role() + "|rid=" + entry.rid()
            + "|" + entry.value();
    }

    private static void require(boolean condition, String message) {
        if (!condition) {
            throw new IllegalStateException(message);
        }
    }

    private static void pause() {
        try {
            Thread.sleep(50);
        } catch (InterruptedException error) {
            Thread.currentThread().interrupt();
            throw new IllegalStateException("interrupted while waiting for evidence", error);
        }
    }

    private static String id(String prefix) {
        return prefix + "-" + UUID.randomUUID().toString().replace("-", "");
    }

    private record WeightedResult(int first, int second) {
    }
}
