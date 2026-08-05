package systems.zlink.e2e.resiliencelifecycle.client.Support;

import com.fasterxml.jackson.databind.JsonNode;
import com.fasterxml.jackson.databind.ObjectMapper;
import java.io.IOException;
import java.nio.file.Files;
import java.nio.file.Path;
import java.net.URI;
import java.time.Duration;
import java.util.HashSet;
import java.util.Set;
import java.util.concurrent.CompletionStage;
import java.util.concurrent.TimeUnit;
import systems.zlink.e2e.resiliencelifecycle.shared.Contracts;
import systems.zlink.httpclient.RawHttpResponse;
import systems.zlink.httpclient.ZLinkHttpClient;

public final class ResilienceScenarioContext {
    private final String endpoint;
    private final String adminAEndpoint;
    private final ObjectMapper json;
    private final ClientOptions options;

    public ResilienceScenarioContext(String endpoint, String adminAEndpoint, ClientOptions options) {
        this.endpoint = endpoint;
        this.adminAEndpoint = adminAEndpoint;
        this.json = new ObjectMapper();
        this.options = options;
    }

    public ClientOptions options() { return options; }

    public Set<String> collectProviders(String prefix, int attempts, int expectedCount) {
        Set<String> providers = new HashSet<>();
        for (int index = 0; index < attempts && providers.size() < expectedCount; index++) {
            Contracts.WorkRes reply = request(prefix + "-" + index, Duration.ofSeconds(3));
            ensure(reply.value().equals("work:" + prefix + "-" + index),
                "reply payload mismatch for " + prefix + "-" + index);
            providers.add(reply.providerRid());
        }
        return providers;
    }

    public Set<String> collectProvidersExactly(String prefix, int attempts) {
        Set<String> providers = new HashSet<>();
        for (int index = 0; index < attempts; index++) {
            String value = prefix + "-" + index;
            Contracts.WorkRes reply = request(value, Duration.ofSeconds(3));
            ensure(reply.value().equals("work:" + value),
                "reply payload mismatch for " + value);
            providers.add(reply.providerRid());
        }
        return providers;
    }

    public Set<String> collectStableProvidersWithout(
        String prefix,
        String forbidden,
        String required) {
        for (int window = 0; window < 30; window++) {
            Set<String> providers = collectProviders(prefix + "-window-" + window, 5, 1);
            if (!providers.contains(forbidden) && providers.contains(required)) {
                return providers;
            }
            sleep(300);
        }
        throw new IllegalStateException(
            prefix + " did not converge away from " + forbidden + " to " + required);
    }

    public Set<String> collectStableProvidersWithoutFailures(
        String prefix,
        String forbidden,
        String required) {
        for (int window = 0; window < 30; window++) {
            Set<String> providers = new HashSet<>();
            for (int index = 0; index < 5; index++) {
                String value = prefix + "-window-" + window + "-" + index;
                try {
                    Contracts.WorkRes reply = request(value, Duration.ofSeconds(3));
                    ensure(reply.value().equals("work:" + value),
                        "reply payload mismatch for " + value);
                    providers.add(reply.providerRid());
                } catch (RuntimeException ignored) {
                }
            }
            if (!providers.contains(forbidden) && providers.contains(required)) {
                return providers;
            }
            sleep(300);
        }
        throw new IllegalStateException(
            prefix + " did not converge away from " + forbidden + " to " + required);
    }

    public void waitForTopology(int expectedRouters) {
        long deadline = System.nanoTime() + TimeUnit.SECONDS.toNanos(10);
        while (System.nanoTime() < deadline) {
            try {
                long count = peers().stream()
                    .count();
                if (count >= expectedRouters) {
                    return;
                }
            } catch (Exception ignored) {
            }
            sleep(200);
        }
        throw new IllegalStateException("registry topology did not report " + expectedRouters
            + " routers for " + Contracts.CHANNEL);
    }

    public void waitForTopologyWithout(String routingId, int seconds) {
        long deadline = System.nanoTime() + TimeUnit.SECONDS.toNanos(seconds);
        while (System.nanoTime() < deadline) {
            try {
                boolean found = peers().stream()
                    .anyMatch(entry -> routingId.equals(entry.routingId()));
                if (!found) {
                    return;
                }
            } catch (Exception ignored) {
            }
            sleep(200);
        }
        throw new IllegalStateException("registry topology still reported " + routingId);
    }

    public void waitForTopologyEndpoint(String routingId, String endpoint) {
        long deadline = System.nanoTime() + TimeUnit.SECONDS.toNanos(15);
        while (System.nanoTime() < deadline) {
            try {
                boolean found = peers().stream()
                    .anyMatch(entry -> routingId.equals(entry.routingId())
                        && endpoint.equals(entry.endpoint()));
                if (found) {
                    return;
                }
            } catch (Exception ignored) {
            }
            sleep(200);
        }
        throw new IllegalStateException(
            "registry topology did not report " + routingId + " at " + endpoint);
    }

    public long peerGeneration(String routingId, String expectedEndpoint) {
        return peers().stream()
            .filter(peer -> routingId.equals(peer.routingId())
                && expectedEndpoint.equals(peer.endpoint()))
            .mapToLong(Contracts.PeerLocation::generation)
            .findFirst()
            .orElseThrow(() -> new IllegalStateException(
                "registry topology did not report " + routingId + " at " + expectedEndpoint));
    }

    public void waitForReplacementGeneration(
        String routingId,
        String endpoint,
        long previousGeneration) {
        long deadline = System.nanoTime() + TimeUnit.SECONDS.toNanos(15);
        while (System.nanoTime() < deadline) {
            try {
                if (peerGeneration(routingId, endpoint) > previousGeneration) {
                    return;
                }
            } catch (RuntimeException ignored) {
            }
            sleep(200);
        }
        throw new IllegalStateException(
            "registry topology did not report a new generation for " + routingId + " at " + endpoint);
    }

    public void waitForProviderEvidence(String marker) {
        long deadline = System.nanoTime() + TimeUnit.SECONDS.toNanos(15);
        while (System.nanoTime() < deadline) {
            if (providerEvidenceContains(adminA(), marker) || providerEvidenceContains(adminB(), marker)) {
                return;
            }
            sleep(100);
        }
        throw new IllegalStateException("provider evidence did not contain " + marker);
    }

    private boolean providerEvidenceContains(String baseUrl, String expected) {
        try {
            JsonNode entries = json.readTree(get(baseUrl + "/evidence")).path("entries");
            if (!entries.isArray()) {
                return false;
            }
            for (JsonNode entry : entries) {
                String value = entry.path("value").asText();
                if (value.contains(expected)) {
                    return true;
                }
            }
        } catch (Exception ignored) {
        }
        return false;
    }

    private java.util.List<Contracts.PeerLocation> peers() {
        return ZLinkHttpClient.create(endpoint).get("/operations/peers")
            .submit(Contracts.PeerSnapshot.class).toCompletableFuture().join().body()
            .peers();
    }

    public void waitForWeight(String baseUrl, int expected) {
        long deadline = System.nanoTime() + TimeUnit.SECONDS.toNanos(5);
        while (System.nanoTime() < deadline) {
            try {
                JsonNode node = json.readTree(get(baseUrl + "/admin/weight"));
                if (node.path("weight").asInt(-1) == expected) {
                    return;
                }
            } catch (Exception ignored) {
            }
            sleep(100);
        }
        throw new IllegalStateException("weight did not become " + expected + " for " + baseUrl);
    }

    public void waitForEvidence(String baseUrl, String marker) {
        long deadline = System.nanoTime() + TimeUnit.SECONDS.toNanos(10);
        while (System.nanoTime() < deadline) {
            try {
                JsonNode entries = json.readTree(get(baseUrl + "/evidence")).path("entries");
                if (entries.isArray()) {
                    for (JsonNode entry : entries) {
                        if (marker.equals(entry.path("marker").asText())) {
                            return;
                        }
                    }
                }
            } catch (Exception ignored) {
            }
            sleep(100);
        }
        throw new IllegalStateException("marker " + marker + " was not observed at " + baseUrl);
    }

    public void waitForEvidenceAny(String marker, String... baseUrls) {
        long deadline = System.nanoTime() + TimeUnit.SECONDS.toNanos(10);
        while (System.nanoTime() < deadline) {
            for (String baseUrl : baseUrls) {
                try {
                    JsonNode entries = json.readTree(get(baseUrl + "/evidence")).path("entries");
                    if (entries.isArray()) {
                        for (JsonNode entry : entries) {
                            if (marker.equals(entry.path("marker").asText())) {
                                return;
                            }
                        }
                    }
                } catch (Exception ignored) {
                }
            }
            sleep(100);
        }
        throw new IllegalStateException("marker " + marker + " was not observed at any provider");
    }

    public void waitForEvidenceValueAny(String marker, String value, String... baseUrls) {
        long deadline = System.nanoTime() + TimeUnit.SECONDS.toNanos(10);
        while (System.nanoTime() < deadline) {
            for (String baseUrl : baseUrls) {
                if (hasEvidence(baseUrl, marker, value)) {
                    return;
                }
            }
            sleep(100);
        }
        throw new IllegalStateException(
            "evidence " + marker + "/" + value + " was not observed at any provider");
    }

    public void waitForDispatchErrorAny(String packetName, String... baseUrls) {
        long deadline = System.nanoTime() + TimeUnit.SECONDS.toNanos(30);
        while (System.nanoTime() < deadline) {
            for (String baseUrl : baseUrls) {
                try {
                    JsonNode entries = json.readTree(get(baseUrl + "/evidence")).path("entries");
                    if (entries.isArray()) {
                        for (JsonNode entry : entries) {
                            String marker = entry.path("marker").asText();
                            String value = entry.path("value").asText();
                            if ("DispatchError".equals(marker)
                                && value.contains("HANDLER_MISSING")
                                && value.contains("REPLY_ERROR")
                                && value.contains(packetName)) {
                                return;
                            }
                        }
                    }
                } catch (Exception ignored) {
                }
            }
            sleep(100);
        }
        throw new IllegalStateException(
            "dispatch error marker for " + packetName + " was not observed");
    }

    public void driveUntilEvidence(String baseUrl, String prefix, String failureMessage) {
        for (int index = 0; index < 80; index++) {
            String value = prefix + "-" + index;
            Contracts.WorkRes reply = request(value, Duration.ofSeconds(3));
            ensure("work:".concat(value).equals(reply.value()),
                "RL-A4 reply payload mismatch for " + value);
            if ("api-b".equals(reply.providerRid()) && hasEvidence(baseUrl, "WorkReq", value)) {
                return;
            }
            sleep(100);
        }
        throw new IllegalStateException(failureMessage);
    }

    public boolean hasEvidence(String baseUrl, String marker, String value) {
        try {
            JsonNode entries = json.readTree(get(baseUrl + "/evidence")).path("entries");
            if (!entries.isArray()) {
                return false;
            }
            for (JsonNode entry : entries) {
                if (marker.equals(entry.path("marker").asText())
                    && value.equals(entry.path("value").asText())) {
                    return true;
                }
            }
        } catch (Exception ignored) {
        }
        return false;
    }

    public boolean hasEvidenceWithPrefix(String baseUrl, String marker, String valuePrefix) {
        try {
            JsonNode entries = json.readTree(get(baseUrl + "/evidence")).path("entries");
            ensure(entries.isArray(), "provider evidence response has no entries array");
            for (JsonNode entry : entries) {
                if (marker.equals(entry.path("marker").asText())
                    && entry.path("value").asText().startsWith(valuePrefix)) {
                    return true;
                }
            }
        } catch (IOException error) {
            throw new IllegalStateException("failed to parse provider evidence", error);
        }
        return false;
    }

    public String adminA() {
        return adminAEndpoint;
    }

    public String adminB() {
        return options.httpBEndpoint();
    }

    public String adminBGreen() {
        return options.httpBGreenEndpoint();
    }

    public void signal(String name) {
        Path dir = controlDir();
        try {
            Files.createDirectories(dir);
            Files.writeString(dir.resolve(name), "ok\n");
        } catch (IOException error) {
            throw new IllegalStateException("failed to write control signal " + name, error);
        }
    }

    public void waitForSignal(String name) {
        Path file = controlDir().resolve(name);
        long deadline = System.nanoTime() + TimeUnit.SECONDS.toNanos(30);
        while (System.nanoTime() < deadline) {
            if (Files.exists(file)) {
                return;
            }
            sleep(100);
        }
        throw new IllegalStateException("control signal was not observed: " + name);
    }

    public boolean hasSignal(String name) {
        return Files.exists(controlDir().resolve(name));
    }

    private Path controlDir() {
        return Path.of(options.controlDir());
    }

    public Contracts.WorkRes request(String value, Duration timeout) {
        return requestAsync(value, timeout).toCompletableFuture().join();
    }

    public CompletionStage<Contracts.WorkRes> requestAsync(String value, Duration timeout) {
        return ZLinkHttpClient.create(endpoint).post("/operations/request/work")
            .timeout(timeout.plusSeconds(5))
            .body(new Contracts.WorkOperation(value, timeout.toMillis()))
            .submit(Contracts.WorkRes.class)
            .thenApply(response -> response.body());
    }

    public void requestUnhandled(String value, Duration timeout) {
        ZLinkHttpClient.create(endpoint).post("/operations/request/unhandled")
            .timeout(timeout.plusSeconds(5))
            .body(new Contracts.UnhandledOperation(value, timeout.toMillis()))
            .submit(Contracts.WorkRes.class).toCompletableFuture().join().body();
    }

    public void send(String value) {
        RawHttpResponse response = ZLinkHttpClient.create(endpoint).post("/operations/send/work")
            .body(new Contracts.WorkMsg(value))
            .submitRaw()
            .toCompletableFuture()
            .join();
        ensure(response.status() >= 200 && response.status() < 300,
            "consumer send operation returned " + response.status());
    }

    public String get(String url) {
        HttpTarget target = HttpTarget.from(url);
        RawHttpResponse response = ZLinkHttpClient.create(target.baseUrl())
            .timeout(Duration.ofSeconds(5))
            .get(target.path())
            .submitRaw()
            .toCompletableFuture()
            .join();
        ensure(response.status() >= 200 && response.status() < 300,
            "GET " + url + " returned " + response.status());
        return response.body();
    }

    public void post(String url) {
        HttpTarget target = HttpTarget.from(url);
        RawHttpResponse response = ZLinkHttpClient.create(target.baseUrl())
            .timeout(Duration.ofSeconds(5))
            .post(target.path())
            .submitRaw()
            .toCompletableFuture()
            .join();
        ensure(response.status() >= 200 && response.status() < 300,
            "POST " + url + " returned " + response.status());
    }

    public CompletionStage<JsonNode> postJsonAsync(String url) {
        HttpTarget target = HttpTarget.from(url);
        return ZLinkHttpClient.create(target.baseUrl())
            .timeout(Duration.ofSeconds(35))
            .post(target.path())
            .submitRaw()
            .thenApply(response -> parseJsonResponse(url, response));
    }

    private JsonNode parseJsonResponse(String url, RawHttpResponse response) {
        ensure(response.status() >= 200 && response.status() < 300,
            "POST " + url + " returned " + response.status());
        try {
            return json.readTree(response.body());
        } catch (IOException error) {
            throw new IllegalStateException("failed to parse POST response from " + url, error);
        }
    }

    public static void sleep(long millis) {
        try {
            Thread.sleep(millis);
        } catch (InterruptedException error) {
            Thread.currentThread().interrupt();
            throw new IllegalStateException("interrupted", error);
        }
    }

    public static void ensure(boolean condition, String message) {
        if (!condition) {
            throw new IllegalStateException(message);
        }
    }

    private record HttpTarget(String baseUrl, String path) {
        static HttpTarget from(String value) {
            URI uri = URI.create(value);
            String target = uri.getRawPath();
            if (uri.getRawQuery() != null) {
                target += "?" + uri.getRawQuery();
            }
            return new HttpTarget(uri.getScheme() + "://" + uri.getRawAuthority(), target);
        }
    }
}
