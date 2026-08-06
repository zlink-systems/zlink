package systems.zlink.e2e.pubsub.client.Scenarios;

import com.fasterxml.jackson.databind.JsonNode;
import java.net.URI;
import java.net.URLEncoder;
import java.nio.charset.StandardCharsets;
import java.time.Duration;
import java.util.HashSet;
import java.util.Set;
import java.util.function.BooleanSupplier;
import systems.zlink.e2e.pubsub.client.Support.NetworkFaultProxy;
import systems.zlink.e2e.pubsub.client.Support.PublisherClient;
import systems.zlink.e2e.pubsub.client.Support.ScenarioAssert;
import systems.zlink.e2e.pubsub.client.Support.ScenarioContext;
import systems.zlink.e2e.pubsub.client.Support.ServerProcessLauncher.ManagedProcess;
import systems.zlink.e2e.pubsub.shared.Contracts;

public final class CommonPubSubScenarios {
    private static final Duration TIMEOUT = Duration.ofSeconds(30);

    private CommonPubSubScenarios() { }

    // PS-D1: Endpoint 없이 automatic publisher descriptor를 발견한다.
    public static void runD1(ScenarioContext context) {
        waitReady(context, context.options().sub1Http(), 1);
        context.publisher().publish("all", event("ps-d1", 1, "automatic-discovery"));
        context.evidence().waitForEvent("sub-1", "ps-d1", 1);
        require(!publisherIds(context, context.options().sub1Http()).isEmpty(),
            "PS-D1 status omitted the ready publisher identity");
        passed("PS-D1");
    }

    // PS-D2: 다른 ChannelName의 publisher를 status와 dispatch에서 제외한다.
    public static void runD2(ScenarioContext context) {
        waitReady(context, context.options().sub1Http(), 1);
        context.publisherAt(required(context.options().auditPublisherHttp(), "auditPublisherHttp"))
            .publish("all", event("ps-d2-audit", 1, "audit-channel"));
        ScenarioAssert.sleep(500);
        require(!hasEvent(context.evidence().snapshot("sub-1"), "ps-d2-audit", 1),
            "PS-D2 received another ChannelName");
        context.publisher().publish("all", event("ps-d2", 1, "events-channel"));
        context.evidence().waitForEvent("sub-1", "ps-d2", 1);
        require(!publisherIds(context, context.options().sub1Http()).contains("audit-publisher"),
            "PS-D2 status included another ChannelName");
        passed("PS-D2");
    }

    // PS-D3: publisher 추가와 정상 제거에 current set이 수렴한다.
    public static void runD3(ScenarioContext context) {
        try (ManagedProcess ignored = startPublisher2(context)) {
            waitReady(context, context.options().sub1Http(), 2);
            context.publisher().publish("all", event("ps-d3-a", 1, "publisher-a"));
            publisher2(context).publish("all", event("ps-d3-b", 1, "publisher-b"));
            context.evidence().waitForEvent("sub-1", "ps-d3-a", 1);
            context.evidence().waitForEvent("sub-1", "ps-d3-b", 1);
            context.publisher().shutdown();
            waitReady(context, context.options().sub1Http(), 1);
            publisher2(context).publish("all", event("ps-d3-after", 1, "publisher-b-after"));
            context.evidence().waitForEvent("sub-1", "ps-d3-after", 1);
        }
        passed("PS-D3");
    }

    // PS-D4: crash한 publisher를 lease 만료 뒤 replacement로 바꾼다.
    public static void runD4(ScenarioContext context) {
        Long pid = context.options().publisherPid();
        require(pid != null && ProcessHandle.of(pid).isPresent(), "PS-D4 publisher PID is missing");
        ProcessHandle.of(pid).orElseThrow().destroyForcibly();
        waitReady(context, context.options().sub1Http(), 0);
        try (ManagedProcess ignored = startPublisher2(context)) {
            waitReady(context, context.options().sub1Http(), 1);
            publisher2(context).publish("all", event("ps-d4", 1, "replacement"));
            context.evidence().waitForEvent("sub-1", "ps-d4", 1);
        }
        passed("PS-D4");
    }

    // PS-D5: Store 장애 중 기존 transport를 유지하고 복구한다.
    public static void runD5(ScenarioContext context, boolean recovered) {
        context.publisher().publish("all", event("ps-d5", recovered ? 2 : 1,
            recovered ? "store-recovered" : "store-paused"));
        context.evidence().waitForEvent("sub-1", "ps-d5", recovered ? 2 : 1);
        if (recovered) waitReady(context, context.options().sub1Http(), 1);
        passed(recovered ? "PS-D5-RECOVERY" : "PS-D5");
    }

    // PS-D6: port 0 재시작 뒤 새 descriptor endpoint로 다시 연결한다.
    public static void runD6(ScenarioContext context) {
        waitReady(context, context.options().sub1Http(), 1);
        String first = listenerEndpoint(context, context.options().publisherHttp());
        context.publisher().shutdown();
        eventually(() -> !context.isHealthy(context.options().publisherHttp()), "publisher A shutdown");
        try (ManagedProcess ignored = startPublisher2(context)) {
            String second = listenerEndpoint(context, context.options().publisher2Http());
            require(!first.equals(second) && !first.endsWith(":0") && !second.endsWith(":0"),
                "PS-D6 listener endpoint did not change from port 0");
            waitReady(context, context.options().sub1Http(), 1);
            publisher2(context).publish("all", event("ps-d6", 2, "port-zero-replacement"));
            context.evidence().waitForEvent("sub-1", "ps-d6", 2);
        }
        passed("PS-D6");
    }

    // PS-D7A: 느린 status observer와 정상 observer, handler를 격리한다.
    public static void runD7A(ScenarioContext context) {
        String sub = context.options().sub1Http();
        waitReady(context, sub, 1);
        context.post(sub + "/observer/start?name=slow&capacity=1&slow=true");
        context.get(sub + "/observer/wait?name=slow&timeoutMs=30000");
        try (ManagedProcess ignored = startPublisher2(context)) {
            waitReady(context, sub, 2);
            context.post(sub + "/observer/start?name=normal&capacity=1&slow=false");
            context.get(sub + "/observer/wait?name=normal&timeoutMs=30000");
            publisher2(context).publish("all", event("ps-d7a", 1, "observer-isolation"));
            context.evidence().waitForEvent("sub-1", "ps-d7a", 1);
            JsonNode evidence = json(context, sub + "/observer/evidence");
            long slowCount = evidence.findValuesAsText("observer").stream()
                .filter("slow"::equals).count();
            require(slowCount == 1, "PS-D7A slow observer exceeded capacity");
            context.post(sub + "/observer/release?name=slow");
            context.post(sub + "/observer/cancel?name=slow");
        }
        passed("PS-D7A");
    }

    // PS-D7B: manual endpoint mutation은 automatic status를 바꾸지 않는다.
    public static void runD7B(ScenarioContext context) {
        try (ManagedProcess ignored = startPublisher2(context)) {
            waitReady(context, context.options().sub1Http(), 2);
            Set<String> before = publisherIds(context, context.options().sub1Http());
            String endpoint = encode(context.options().publisher2Endpoint());
            context.post(context.options().sub4Http() + "/connections?operation=connect&endpoint=" + endpoint);
            context.post(context.options().sub4Http() + "/connections?operation=disconnect&endpoint=" + endpoint);
            context.publisher().publish("all", event("ps-d7b", 1, "automatic-before"));
            context.evidence().waitForEvent("sub-1", "ps-d7b", 1);
            publisher2(context).publish("all", event("ps-d7b", 2, "automatic-after"));
            context.evidence().waitForEvent("sub-1", "ps-d7b", 2);
            require(before.equals(publisherIds(context, context.options().sub1Http())),
                "PS-D7B manual mutation changed automatic status");
        }
        passed("PS-D7B");
    }

    // PS-E1: Store 없이 manual endpoint subscriber가 typed event를 받는다.
    public static void runE1(ScenarioContext context) {
        context.publisher().publish("all", event("ps-e1", 1, "manual-without-store"));
        context.evidence().waitForEventAt(context.options().sub4Http(), "ps-e1", 1);
        passed("PS-E1");
    }

    // PS-F1: automatic과 manual publisher가 각각 Ready delivery를 제공한다.
    public static void runF1(ScenarioContext context) {
        waitReady(context, context.options().sub1Http(), 1);
        try (ManagedProcess ignored = startPublisher2(context)) {
            waitReady(context, context.options().sub4Http(), 1);
            publisher2(context).publish("all", event("ps-f1", 1, "manual-publisher"));
            context.evidence().waitForEventAt(context.options().sub4Http(), "ps-f1", 1);
            context.publisher().publish("all", event("ps-f1", 2, "automatic-publisher"));
            context.evidence().waitForEvent("sub-1", "ps-f1", 2);
        }
        passed("PS-F1");
    }

    // PS-F2: publisher B의 수신 경로만 차단하고 A의 Ready와 delivery를 유지한다.
    public static void runF2(ScenarioContext context) {
        URI upstream = URI.create(context.options().publisher2Endpoint());
        try (NetworkFaultProxy fault = NetworkFaultProxy.start(upstream.getHost(), upstream.getPort());
             ManagedProcess ignored = startPublisher2(context)) {
            waitReady(context, context.options().sub1Http(), 2);
            fault.block();
            eventually(() -> publisherIds(context, context.options().sub1Http())
                .equals(Set.of("publisher-a")), "publisher B not-ready");
            context.publisher().publish("all", event("ps-f2", 1, "publisher-a-during-b-failure"));
            context.evidence().waitForEvent("sub-1", "ps-f2", 1);
            fault.unblock();
            waitReady(context, context.options().sub1Http(), 2);
            publisher2(context).publish("all", event("ps-f2", 2, "publisher-b-after-recovery"));
            context.evidence().waitForEvent("sub-1", "ps-f2", 2);
        }
        passed("PS-F2");
    }

    // PS-F3: exact reserved topic만 public argument error로 거부한다.
    public static void runF3(ScenarioContext context) {
        int status = context.publisher().publishReservedStatus();
        require(status >= 400 && status < 500, "PS-F3 reserved topic was accepted");
        context.publisher().publishReservedPrefix();
        context.evidence().waitForEvent("sub-1", "ps-f3", 2);
        passed("PS-F3");
    }

    // PS-F4: orderly disconnect를 15초 deadline 전에 ready set에서 제거한다.
    public static void runF4(ScenarioContext context) {
        try (ManagedProcess ignored = startPublisher2(context)) {
            waitReady(context, context.options().sub1Http(), 2);
            context.publisher().shutdown();
            waitReady(context, context.options().sub1Http(), 1);
            publisher2(context).publish("all", event("ps-f4", 1, "remaining-publisher"));
            context.evidence().waitForEvent("sub-1", "ps-f4", 1);
        }
        passed("PS-F4");
    }

    // PS-F5: 구독하지 않은 traffic이 계속돼도 beacon으로 Ready를 유지한다.
    public static void runF5(ScenarioContext context) {
        waitReady(context, context.options().sub1Http(), 1);
        long deadline = System.nanoTime() + Duration.ofSeconds(17).toNanos();
        int sequence = 1;
        while (System.nanoTime() < deadline) {
            context.publisher().publish("events.a", event("ps-f5-a", sequence++, "unsubscribed"));
            ScenarioAssert.sleep(1_000);
        }
        require(!hasEvent(context.evidence().snapshot("sub-1"), "ps-f5-a", 1),
            "PS-F5 handled an unsubscribed topic");
        require(json(context, context.options().sub1Http() + "/status")
            .path("isReady").asBoolean(false), "PS-F5 publisher became not-ready");
        context.publisher().publish("events.b", event("ps-f5-b", 1, "subscribed"));
        context.evidence().waitForEvent("sub-1", "ps-f5-b", 1);
        passed("PS-F5");
    }

    private static ManagedProcess startPublisher2(ScenarioContext context) {
        var options = context.options();
        return context.processes().startPublisher(
            "publisher-b", options.publisher2Endpoint(), options.publisher2Http(),
            options.publisher2Rid(), Contracts.EVENT_CHANNEL, options.publisher2NoStore(),
            options.publisher2Port(), options.publisher2AdvertiseHost());
    }

    private static PublisherClient publisher2(ScenarioContext context) {
        return context.publisherAt(required(context.options().publisher2Http(), "publisher2Http"));
    }

    private static void waitReady(ScenarioContext context, String endpoint, int count) {
        eventually(() -> {
            JsonNode status = json(context, endpoint + "/status");
            int ready = status.path("readyPublisherCount").asInt(-1);
            return count == 0 ? ready == 0 : status.path("isReady").asBoolean() && ready >= count;
        }, "fanout ready count " + count);
    }

    private static Set<String> publisherIds(ScenarioContext context, String endpoint) {
        Set<String> result = new HashSet<>();
        for (JsonNode publisher : json(context, endpoint + "/status").path("publishers")) {
            if ("READY".equals(publisher.path("state").asText())) {
                result.add(publisher.path("nodeRid").asText());
            }
        }
        return Set.copyOf(result);
    }

    private static String listenerEndpoint(ScenarioContext context, String endpoint) {
        String value = json(context, endpoint + "/status").path("listenerEndpoint").asText();
        require(!value.isBlank(), "publisher status omitted listenerEndpoint");
        return value;
    }

    private static JsonNode json(ScenarioContext context, String url) {
        try {
            return context.json().readTree(context.get(url));
        } catch (Exception error) {
            throw new IllegalStateException("failed to read " + url, error);
        }
    }

    private static boolean hasEvent(Contracts.EvidenceSnapshot snapshot, String scenario, int sequence) {
        return snapshot.entries().stream().anyMatch(entry ->
            "EventMsg".equals(entry.marker()) && scenario.equals(entry.scenario())
                && sequence == entry.sequence());
    }

    private static Contracts.EventMsg event(String scenario, int sequence, String value) {
        return new Contracts.EventMsg(scenario, sequence, value);
    }

    private static void eventually(BooleanSupplier check, String label) {
        long deadline = System.nanoTime() + TIMEOUT.toNanos();
        Throwable last = null;
        while (System.nanoTime() < deadline) {
            try {
                if (check.getAsBoolean()) return;
            } catch (Throwable error) {
                last = error;
            }
            ScenarioAssert.sleep(100);
        }
        throw new IllegalStateException("timed out waiting for " + label, last);
    }

    private static String required(String value, String name) {
        if (value == null || value.isBlank()) throw new IllegalArgumentException(name + " is required");
        return value;
    }

    private static String encode(String value) {
        return URLEncoder.encode(value, StandardCharsets.UTF_8);
    }

    private static void require(boolean condition, String message) {
        if (!condition) throw new IllegalStateException(message);
    }

    private static void passed(String selector) {
        System.out.println("scenario " + selector + " passed");
    }
}
