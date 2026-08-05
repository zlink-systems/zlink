package systems.zlink.e2e.channelegress.client;

import java.util.List;
import java.util.UUID;
import systems.zlink.e2e.channelegress.shared.Contracts;

public final class ScenarioSuite {
    private ScenarioSuite() {
    }

    public static void run(String selector, ClientOptions options) {
        List<String> selected = "all".equalsIgnoreCase(selector)
            ? List.of("CH-E2E-01", "CH-E2E-02", "CH-E2E-11")
            : List.of(selector.split(","));
        for (String raw : selected) {
            String scenario = raw.trim().toUpperCase();
            switch (scenario) {
                case "CH-E2E-01" -> ch01(options);
                case "CH-E2E-02" -> ch02(options);
                case "CH-E2E-11" -> ch11(options);
                default -> throw new IllegalArgumentException(
                    "ChannelEgressRouting scenario is not implemented: " + scenario);
            }
            System.out.println("scenario " + scenario + " passed");
        }
    }

    private static void ch01(ClientOptions options) {
        String id = id("ch-01");
        Contracts.InvokeRes forward = ClientHttp.post(
            options.sessionEndpoint(),
            "/request",
            new Contracts.InvokeReq(Contracts.PLAY_CHANNEL, id),
            Contracts.InvokeRes.class);
        Contracts.InvokeRes reverse = ClientHttp.post(
            options.playEndpoint(),
            "/request",
            new Contracts.InvokeReq(Contracts.SESSION_CHANNEL, id + "-reverse"),
            Contracts.InvokeRes.class);
        ClientHttp.assertTrue(forward.succeeded(), "CH-E2E-01 forward request failed: " + forward.error());
        ClientHttp.assertTrue(reverse.succeeded(), "CH-E2E-01 reverse request failed: " + reverse.error());
        ClientHttp.assertTrue(
            forward.reply() != null && "play".equals(forward.reply().role()),
            "CH-E2E-01 forward reply was not handled by play");
        ClientHttp.assertTrue(
            reverse.reply() != null && "session".equals(reverse.reply().role()),
            "CH-E2E-01 reverse reply was not handled by session");
        waitFor(options.playEndpoint(), id);
        waitFor(options.sessionEndpoint(), id + "-reverse");
    }

    private static void ch02(ClientOptions options) {
        String directId = id("ch-02-direct");
        Contracts.InvokeRes direct = ClientHttp.post(
            options.sessionEndpoint(),
            "/request",
            new Contracts.InvokeReq(Contracts.PLAY_CHANNEL, directId),
            Contracts.InvokeRes.class);
        ClientHttp.assertTrue(direct.succeeded(),
            "CH-E2E-02 direct route failed: " + direct.error());
        String id = id("ch-02");
        Contracts.InvokeRes result = ClientHttp.post(
            options.sessionEndpoint(),
            "/request",
            new Contracts.InvokeReq(Contracts.PLAY_CHANNEL, id, "cascade"),
            Contracts.InvokeRes.class);
        ClientHttp.assertTrue(result.succeeded(), "CH-E2E-02 request failed: " + result.error());
        ClientHttp.assertTrue(
            result.reply() != null && result.reply().downstream().size() == 2,
            "CH-E2E-02 did not return both downstream replies");
        waitFor(options.auditEndpoint(), id + "-audit");
        waitForAny(options.workflowAEndpoint(), options.workflowBEndpoint(), id + "-workflow");
    }

    private static void ch11(ClientOptions options) {
        String id = id("ch-11");
        Contracts.InvokeRes request = ClientHttp.post(
            options.sessionEndpoint(),
            "/request",
            new Contracts.InvokeReq(Contracts.API_CHANNEL, id),
            Contracts.InvokeRes.class);
        Contracts.SendRes send = ClientHttp.post(
            options.sessionEndpoint(),
            "/send",
            new Contracts.InvokeReq(Contracts.API_CHANNEL, id + "-send"),
            Contracts.SendRes.class);
        ClientHttp.assertTrue(request.succeeded(), "CH-E2E-11 request failed: " + request.error());
        ClientHttp.assertTrue(send.succeeded(), "CH-E2E-11 send failed: " + send.error());
        waitForAny(options.apiAEndpoint(), options.apiBEndpoint(), id);
        waitForAny(options.apiAEndpoint(), options.apiBEndpoint(), id + "-send");
    }

    private static void waitFor(String endpoint, String fragment) {
        long deadline = System.nanoTime() + DurationSupport.timeoutNanos();
        while (System.nanoTime() < deadline) {
            Contracts.EvidenceSnapshot snapshot = ClientHttp.get(
                endpoint, "/evidence", Contracts.EvidenceSnapshot.class);
            if (snapshot.entries().stream().anyMatch(entry -> evidenceLine(entry).contains(fragment))) {
                return;
            }
            DurationSupport.pause();
        }
        throw new IllegalStateException("timed out waiting for evidence " + fragment + " at " + endpoint);
    }

    private static void waitForAny(String first, String second, String fragment) {
        long deadline = System.nanoTime() + DurationSupport.timeoutNanos();
        while (System.nanoTime() < deadline) {
            for (String endpoint : List.of(first, second)) {
                Contracts.EvidenceSnapshot snapshot = ClientHttp.get(
                    endpoint, "/evidence", Contracts.EvidenceSnapshot.class);
                if (snapshot.entries().stream().anyMatch(entry -> evidenceLine(entry).contains(fragment))) {
                    return;
                }
            }
            DurationSupport.pause();
        }
        throw new IllegalStateException("timed out waiting for evidence " + fragment);
    }

    private static String evidenceLine(Contracts.EvidenceEntry entry) {
        return entry.marker() + "|role=" + entry.role() + "|rid=" + entry.rid()
            + "|" + entry.value();
    }

    private static String id(String prefix) {
        return prefix + "-" + UUID.randomUUID().toString().replace("-", "");
    }

    private static final class DurationSupport {
        private DurationSupport() {
        }

        static long timeoutNanos() {
            return java.time.Duration.ofSeconds(20).toNanos();
        }

        static void pause() {
            try {
                Thread.sleep(50);
            } catch (InterruptedException error) {
                Thread.currentThread().interrupt();
                throw new IllegalStateException("interrupted while waiting for evidence", error);
            }
        }
    }
}
