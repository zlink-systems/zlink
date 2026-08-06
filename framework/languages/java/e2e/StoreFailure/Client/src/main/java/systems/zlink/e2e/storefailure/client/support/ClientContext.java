package systems.zlink.e2e.storefailure.client.support;

import com.fasterxml.jackson.databind.JsonNode;
import com.fasterxml.jackson.databind.ObjectMapper;
import java.time.Duration;
import java.time.Instant;
import java.util.ArrayList;
import java.util.Collections;
import java.util.HashSet;
import java.util.List;
import java.util.Set;
import systems.zlink.e2e.storefailure.shared.Contracts;
import systems.zlink.e2e.storefailure.shared.Wait;

public final class ClientContext implements AutoCloseable {
    private static final ObjectMapper JSON = new ObjectMapper();

    private final ClientOptions options;
    private final StoreFailureHttpClient http;

    public ClientContext(ClientOptions options) {
        if (options.consumerHttpEndpoint() == null || options.consumerHttpEndpoint().isBlank()) {
            throw new IllegalArgumentException("consumer HTTP endpoint is required");
        }
        this.options = options;
        this.http = new StoreFailureHttpClient(options.consumerHttpEndpoint());
    }

    public ClientOptions options() {
        return options;
    }

    public DiscoveryApiResult requestUntilAnyProvider() {
        return requestUntilAnyProvider("SF-A1", "sf-a1", 60);
    }

    public DiscoveryApiResult requestUntilAnyProvider(String scenarioName, String markerPrefix, int attempts) {
        for (int index = 0; index < attempts; index++) {
            Contracts.ProfileReq request =
                new Contracts.ProfileReq(markerPrefix + "-msg-" + index, markerPrefix + "-marker-" + index);
            try {
                String body = http.postJson("/profile/request/wait", request);
                Contracts.ProfileRes reply = JSON.readValue(body, Contracts.ProfileRes.class);
                ScenarioAssert.that(reply.value().equals("profile:" + markerPrefix + "-msg-" + index),
                    scenarioName + " reply payload mismatch");
                ScenarioAssert.that(options.expectedRids().contains(reply.providerRid()),
                    scenarioName + " unexpected provider " + reply.providerRid());
                return new DiscoveryApiResult(Set.of(reply.providerRid()));
            } catch (Exception error) {
                if (index == attempts - 1) {
                    throw new IllegalStateException(scenarioName + " consumer request failed", error);
                }
            }
        }
        throw new IllegalStateException(scenarioName + " consumer request did not complete");
    }

    public DiscoveryApiResult requestReplacementLifecycle(
        String scenarioName,
        String markerPrefix,
        int count) {
        ScenarioAssert.that(!options.expectedLifecycleId().isBlank(),
            scenarioName + " expected lifecycle id is required");
        Set<String> providers = new HashSet<>();
        for (int index = 0; index < count; index++) {
            Contracts.ProfileReq request = new Contracts.ProfileReq(
                markerPrefix + "-msg-" + index,
                markerPrefix + "-marker-" + index);
            Contracts.ProfileRes reply = index == 0
                ? awaitReplacementProfile(scenarioName, request)
                : requestProfile(scenarioName, request);
            ScenarioAssert.that(options.expectedRids().contains(reply.providerRid()),
                scenarioName + " unexpected provider " + reply.providerRid());
            ScenarioAssert.that(options.expectedLifecycleId().equals(reply.providerLifecycle()),
                scenarioName + " request reached lifecycle " + reply.providerLifecycle()
                    + " instead of " + options.expectedLifecycleId());
            providers.add(reply.providerRid());
        }
        return new DiscoveryApiResult(Set.copyOf(providers));
    }

    private Contracts.ProfileRes awaitReplacementProfile(
        String scenarioName,
        Contracts.ProfileReq request) {
        RuntimeException lastError = null;
        for (int attempt = 0; attempt < 20; attempt++) {
            try {
                Contracts.ProfileRes reply = requestProfile(scenarioName, request);
                if (options.expectedLifecycleId().equals(reply.providerLifecycle())) {
                    return reply;
                }
                lastError = new IllegalStateException(
                    scenarioName + " still selected lifecycle " + reply.providerLifecycle());
            } catch (RuntimeException error) {
                lastError = error;
            }
            Wait.sleep(Duration.ofMillis(200));
        }
        throw new IllegalStateException(
            scenarioName + " replacement lifecycle did not become selectable",
            lastError);
    }

    private Contracts.ProfileRes requestProfile(
        String scenarioName,
        Contracts.ProfileReq request) {
        try {
            return JSON.readValue(
                http.postJson("/profile/request/wait", request),
                Contracts.ProfileRes.class);
        } catch (Exception error) {
            throw new IllegalStateException(scenarioName + " replacement request failed", error);
        }
    }

    public DiscoveryApiResult requestInstanceOwner(
        String scenarioName,
        String spotId,
        String markerPrefix,
        int count) {
        ScenarioAssert.that(!options.expectedLifecycleId().isBlank(),
            scenarioName + " expected lifecycle id is required");
        Set<String> providers = new HashSet<>();
        for (int index = 0; index < count; index++) {
            Contracts.InstanceReq request =
                new Contracts.InstanceReq(spotId, markerPrefix + "-" + index);
            Contracts.InstanceOutcome outcome = index == 0
                ? awaitInstanceOwner(scenarioName, request)
                : instanceRequest(request);
            ScenarioAssert.that(outcome.succeeded(),
                scenarioName + " instance request failed: " + outcome.errorKind()
                    + " " + outcome.errorMessage());
            Contracts.InstanceRes reply = outcome.reply();
            ScenarioAssert.that(spotId.equals(reply.spotId()),
                scenarioName + " instance spot id mismatch");
            ScenarioAssert.that(options.expectedLifecycleId().equals(reply.ownerLifecycle()),
                scenarioName + " instance request reached lifecycle " + reply.ownerLifecycle()
                    + " instead of " + options.expectedLifecycleId());
            ScenarioAssert.that(reply.objectGeneration() > 0,
                scenarioName + " instance object generation is not positive");
            providers.add(reply.ownerRid());
            if (index == 0 && count > 1) {
                // The cold activation reply can precede publication of its
                // current authority row. Follow-up calls begin only after one
                // configured polling interval has elapsed.
                Wait.sleep(Duration.ofMillis(options.locationPollingMillis()));
            }
        }
        return new DiscoveryApiResult(Set.copyOf(providers));
    }

    private Contracts.InstanceOutcome awaitInstanceOwner(
        String scenarioName,
        Contracts.InstanceReq request) {
        Contracts.InstanceOutcome last = null;
        for (int attempt = 0; attempt < 40; attempt++) {
            last = instanceRequest(request);
            if (last.succeeded()
                && options.expectedLifecycleId().equals(last.reply().ownerLifecycle())) {
                return last;
            }
            Wait.sleep(Duration.ofMillis(200));
        }
        throw new IllegalStateException(
            scenarioName + " instance replacement did not become selectable; last=" + last);
    }

    public DiscoveryApiResult requestInstanceUnavailable(
        String scenarioName,
        String spotId) {
        Contracts.InstanceOutcome outcome = instanceRequest(
            new Contracts.InstanceReq(spotId, "sf-b3-after-lease"));
        ScenarioAssert.that(!outcome.succeeded(),
            scenarioName + " accepted a new stateful request after owner lease expiry");
        ScenarioAssert.that("UNAVAILABLE".equals(outcome.errorKind()),
            scenarioName + " expected UNAVAILABLE but received " + outcome.errorKind()
                + ": " + outcome.errorMessage());
        return new DiscoveryApiResult(Set.of());
    }

    private Contracts.InstanceOutcome instanceRequest(Contracts.InstanceReq request) {
        try {
            return JSON.readValue(
                http.postJson("/instance/request", request),
                Contracts.InstanceOutcome.class);
        } catch (Exception error) {
            throw new IllegalStateException("instance request endpoint failed", error);
        }
    }

    public void createObjectLocationFixture(String scenarioName, int count) {
        for (int index = 0; index < count; index++) {
            String spotId = "sf-c5-object-" + index;
            Contracts.InstanceOutcome outcome = null;
            RuntimeException lastError = null;
            for (int attempt = 0; attempt < 20; attempt++) {
                try {
                    outcome = instanceRequest(new Contracts.InstanceReq(spotId, scenarioName + "-" + index));
                    if (outcome.succeeded()) {
                        break;
                    }
                } catch (RuntimeException error) {
                    lastError = error;
                }
                Wait.sleep(Duration.ofMillis(100));
            }
            ScenarioAssert.that(outcome != null && outcome.succeeded(),
                scenarioName + " object fixture request failed at " + index
                    + ": " + (outcome == null ? lastError : outcome.errorMessage()));
            ScenarioAssert.that(spotId.equals(outcome.reply().spotId()),
                scenarioName + " object fixture returned the wrong spot id at " + index);
            ScenarioAssert.that(outcome.reply().objectGeneration() > 0,
                scenarioName + " object fixture returned a non-positive generation at " + index);
        }
    }

    public void assertObjectLocationPaging(String scenarioName, int expectedCount) {
        for (int pageSize : List.of(1, 100, 1_000)) {
            Set<String> observed = Wait.until(
                Duration.ofSeconds(30),
                scenarioName + " did not publish all object locations for page size " + pageSize,
                () -> {
                    try {
                        Set<String> pageResult = readObjectLocationIds(pageSize, expectedCount);
                        return pageResult.size() == expectedCount ? pageResult : null;
                    } catch (RuntimeException error) {
                        return null;
                    }
                });
            ScenarioAssert.that(observed.size() == expectedCount,
                scenarioName + " object page count mismatch for page size " + pageSize);
        }
    }

    private Set<String> readObjectLocationIds(int pageSize, int expectedCount) {
        Set<String> observed = new HashSet<>();
        String continuation = null;
        int pages = 0;
        do {
            String body = http.postJson(
                "/locations/objects",
                new Contracts.ObjectLocationQueryReq(pageSize, continuation));
            try {
                JsonNode root = JSON.readTree(body);
                JsonNode items = root.path("items");
                ScenarioAssert.that(items.isArray(), "SF-C5 object query did not return an item array");
                ScenarioAssert.that(items.size() <= pageSize,
                    "SF-C5 page exceeded requested page size " + pageSize);
                for (JsonNode item : items) {
                    String globalId = item.path("globalId").asText("");
                    ScenarioAssert.that(!globalId.isBlank(), "SF-C5 returned an object without globalId");
                    ScenarioAssert.that(item.path("objectGeneration").asLong(0) > 0,
                        "SF-C5 returned a non-positive object generation");
                    ScenarioAssert.that(Contracts.LEASE_PROBE_SPOT_TYPE.equals(item.path("stableType").asText()),
                        "SF-C5 returned an object with the wrong stable type");
                    ScenarioAssert.that(observed.add(globalId),
                        "SF-C5 returned a duplicate object " + globalId);
                }
                continuation = root.path("continuationToken").asText("");
                continuation = continuation.isBlank() ? null : continuation;
            } catch (Exception error) {
                throw new IllegalStateException("SF-C5 object query response was invalid", error);
            }
            pages++;
            ScenarioAssert.that(pages <= expectedCount,
                "SF-C5 object query did not terminate");
        } while (continuation != null);
        return observed;
    }

    public long measureStoreRead() {
        Instant started = Instant.now();
        waitForLivePeerRows();
        return Duration.between(started, Instant.now()).toMillis();
    }

    public List<Long> measureRequests(String markerPrefix, int count) {
        List<Long> timings = new ArrayList<>(count);
        for (int index = 0; index < count; index++) {
            Contracts.ProfileReq request =
                new Contracts.ProfileReq(markerPrefix + "-msg-" + index, markerPrefix + "-marker-" + index);
            Instant started = Instant.now();
            try {
                String body = http.postJson("/profile/request", request);
                Contracts.ProfileRes reply = JSON.readValue(body, Contracts.ProfileRes.class);
                ScenarioAssert.that(reply.value().equals("profile:" + markerPrefix + "-msg-" + index),
                    "SF-E1 reply payload mismatch");
                ScenarioAssert.that(options.expectedRids().contains(reply.providerRid()),
                    "SF-E1 unexpected provider " + reply.providerRid());
            } catch (Exception error) {
                throw new IllegalStateException("SF-E1 request failed", error);
            }
            timings.add(Duration.between(started, Instant.now()).toMillis());
        }
        return timings;
    }

    public void setStoreDelay(int delayMilliseconds) {
        try {
            http.postJson("/admin/store-delay", new Contracts.StoreDelayReq(delayMilliseconds));
        } catch (Exception error) {
            throw new IllegalStateException("failed to set store delay", error);
        }
    }

    public static long percentileMillis(List<Long> values, double percentile) {
        List<Long> sorted = new ArrayList<>(values);
        Collections.sort(sorted);
        int index = (int) Math.ceil(percentile * sorted.size()) - 1;
        return sorted.get(Math.max(0, Math.min(index, sorted.size() - 1)));
    }

    public DiscoveryApiResult requestSurvivorOnly() {
        return requestSurvivorOnly("SF-C1", "sf-c1", 12);
    }

    public DiscoveryApiResult requestSurvivorOnly(String scenarioName, String markerPrefix, int count) {
        List<String> survivors = options.expectedRids();
        ScenarioAssert.that(!survivors.isEmpty(), scenarioName + " requires at least one survivor rid");
        Set<String> providers = new HashSet<>();
        Instant started = Instant.now();
        for (int index = 0; index < count; index++) {
            Contracts.ProfileReq request =
                new Contracts.ProfileReq(markerPrefix + "-msg-" + index, markerPrefix + "-marker-" + index);
            try {
                String body = http.postJson("/profile/request", request);
                Contracts.ProfileRes reply = JSON.readValue(body, Contracts.ProfileRes.class);
                ScenarioAssert.that(reply.value().equals("profile:" + markerPrefix + "-msg-" + index),
                    scenarioName + " reply payload mismatch");
                ScenarioAssert.that(survivors.contains(reply.providerRid()),
                    scenarioName + " request reached non-survivor provider " + reply.providerRid());
                ScenarioAssert.that(!reply.providerRid().equals(options.deadRid()),
                    scenarioName + " request reached crashed provider " + options.deadRid());
                providers.add(reply.providerRid());
            } catch (Exception error) {
                throw new IllegalStateException(scenarioName + " survivor request failed", error);
            }
        }
        Duration elapsed = Duration.between(started, Instant.now());
        ScenarioAssert.that(elapsed.compareTo(Duration.ofSeconds(12)) < 0,
            scenarioName + " requests were slow, suggesting repeated routing to the dead provider: " + elapsed);
        return new DiscoveryApiResult(Set.copyOf(providers));
    }

    public DiscoveryApiResult driveRequests(String markerPrefix, Duration window, String scenarioName) {
        Set<String> providers = new HashSet<>();
        Instant deadline = Instant.now().plus(window);
        int index = 0;
        while (Instant.now().isBefore(deadline)) {
            Contracts.ProfileReq request =
                new Contracts.ProfileReq(markerPrefix + "-msg-" + index, markerPrefix + "-marker-" + index);
            try {
                String body = http.postJson("/profile/request", request);
                Contracts.ProfileRes reply = JSON.readValue(body, Contracts.ProfileRes.class);
                ScenarioAssert.that(reply.value().equals("profile:" + markerPrefix + "-msg-" + index),
                    scenarioName + " reply payload mismatch");
                ScenarioAssert.that(options.expectedRids().contains(reply.providerRid()),
                    scenarioName + " unexpected provider " + reply.providerRid());
                providers.add(reply.providerRid());
            } catch (Exception error) {
                throw new IllegalStateException(scenarioName + " request failed during outage", error);
            }
            index++;
            Wait.sleep(Duration.ofMillis(150));
        }
        ScenarioAssert.that(index > 0, scenarioName + " produced no request traffic");
        return new DiscoveryApiResult(Set.copyOf(providers));
    }

    public DiscoveryApiResult driveTolerantRequests(String markerPrefix, Duration window, String scenarioName) {
        Set<String> providers = new HashSet<>();
        Instant deadline = Instant.now().plus(window);
        Instant lastSuccess = Instant.now();
        Duration maxGap = Duration.ZERO;
        int index = 0;
        while (Instant.now().isBefore(deadline)) {
            Contracts.ProfileReq request =
                new Contracts.ProfileReq(markerPrefix + "-msg-" + index, markerPrefix + "-marker-" + index);
            try {
                String body = http.postJson("/profile/request", request);
                Contracts.ProfileRes reply = JSON.readValue(body, Contracts.ProfileRes.class);
                ScenarioAssert.that(reply.value().equals("profile:" + markerPrefix + "-msg-" + index),
                    scenarioName + " reply payload mismatch");
                providers.add(reply.providerRid());
                Duration gap = Duration.between(lastSuccess, Instant.now());
                if (gap.compareTo(maxGap) > 0) {
                    maxGap = gap;
                }
                lastSuccess = Instant.now();
            } catch (Exception ignored) {
                // A request may land on the provider killed during the store outage.
            }
            index++;
            Wait.sleep(Duration.ofMillis(150));
        }
        Duration finalGap = Duration.between(lastSuccess, Instant.now());
        if (finalGap.compareTo(maxGap) > 0) {
            maxGap = finalGap;
        }
        ScenarioAssert.that(!providers.isEmpty(), scenarioName + " produced no successful request traffic");
        ScenarioAssert.that(maxGap.compareTo(Duration.ofMillis(options.locationLeaseTtlMillis() * 2)) < 0,
            scenarioName + " successful traffic stalled for " + maxGap);
        return new DiscoveryApiResult(Set.copyOf(providers));
    }

    public void waitForLivePeerRows() {
        Set<String> expectedSet = new HashSet<>(options.expectedRids());
        Set<String> observed = Wait.until(
            Duration.ofSeconds(20),
            "consumer location query missing expected peer rows " + options.expectedRids(),
            () -> {
                try {
                    JsonNode root = JSON.readTree(http.get("/locations/peers"));
                    Set<String> rids = new HashSet<>();
                    for (JsonNode entry : root) {
                        String rid = entry.path("nodeRid").asText("");
                        if (isReadyPeer(entry) && !rid.isBlank()) {
                            rids.add(rid);
                        }
                    }
                    return containsAllRids(rids, expectedSet) ? rids : null;
                } catch (Exception error) {
                    return null;
                }
            });
        ScenarioAssert.that(containsAllRids(observed, expectedSet),
            "location rows missing expected providers " + expectedSet + ": " + observed);
    }

    public void waitForPeerRowsExcluding(String deadRid, Duration timeout) {
        Set<String> survivorSet = new HashSet<>(options.expectedRids());
        Set<String> observed = Wait.until(
            timeout,
            "consumer location query still includes removed provider " + deadRid,
            () -> {
                try {
                    JsonNode root = JSON.readTree(http.get("/locations/peers"));
                    Set<String> rids = new HashSet<>();
                    for (JsonNode entry : root) {
                        String rid = entry.path("nodeRid").asText("");
                        if (isReadyPeer(entry) && !rid.isBlank()) {
                            rids.add(rid);
                        }
                    }
                    boolean hasSurvivors = containsAllRids(rids, survivorSet);
                    boolean hasDead = rids.stream().anyMatch(value -> value.equals(deadRid) || value.contains(deadRid));
                    return hasSurvivors && !hasDead ? rids : null;
                } catch (Exception error) {
                    return null;
                }
            });
        ScenarioAssert.that(containsAllRids(observed, survivorSet),
            "location rows missing survivor providers " + survivorSet + ": " + observed);
        ScenarioAssert.that(observed.stream().noneMatch(value -> value.equals(deadRid) || value.contains(deadRid)),
            "location rows still include crashed provider " + deadRid + ": " + observed);
    }

    public void waitForReadyTargetsExcluding(String excludedRid, Duration timeout) {
        Set<String> expectedSet = new HashSet<>(options.expectedRids());
        Set<String> observed = Wait.until(
            timeout,
            "consumer ready targets did not retain " + expectedSet
                + " while excluding " + excludedRid,
            () -> {
                try {
                    JsonNode root = JSON.readTree(http.get("/mesh/peers"));
                    Set<String> rids = new HashSet<>();
                    for (JsonNode entry : root) {
                        String rid = entry.path("nodeRid").asText("");
                        if ("READY".equals(entry.path("state").asText("")) && !rid.isBlank()) {
                            rids.add(rid);
                        }
                    }
                    boolean hasExpected = containsAllRids(rids, expectedSet);
                    boolean hasExcluded = rids.stream()
                        .anyMatch(value -> value.equals(excludedRid) || value.contains(excludedRid));
                    return hasExpected && !hasExcluded ? rids : null;
                } catch (Exception error) {
                    return null;
                }
            });
        ScenarioAssert.that(containsAllRids(observed, expectedSet),
            "ready targets missing existing providers " + expectedSet + ": " + observed);
        ScenarioAssert.that(observed.stream()
                .noneMatch(value -> value.equals(excludedRid) || value.contains(excludedRid)),
            "ready targets include unverified provider " + excludedRid + ": " + observed);
    }

    public void waitForReadyPeerRow(String expectedRid, Duration timeout) {
        String observed = Wait.until(
            timeout,
            "consumer location query did not include ready provider " + expectedRid,
            () -> {
                try {
                    JsonNode root = JSON.readTree(http.get("/locations/peers"));
                    for (JsonNode entry : root) {
                        String rid = entry.path("nodeRid").asText("");
                        if (isReadyPeer(entry)
                            && (rid.equals(expectedRid) || rid.contains(expectedRid))) {
                            return rid;
                        }
                    }
                    return null;
                } catch (Exception error) {
                    return null;
                }
            });
        ScenarioAssert.that(observed.equals(expectedRid) || observed.contains(expectedRid),
            "ready provider row does not match " + expectedRid + ": " + observed);
    }

    public JsonNode waitForHealthyStatus() {
        long deadline = System.nanoTime() + Duration.ofSeconds(20).toNanos();
        JsonNode status = null;
        Exception lastError = null;
        while (System.nanoTime() < deadline) {
            try {
                status = JSON.readTree(http.get("/locations/status"));
                if (status.path("storeHealthy").asBoolean(false)) {
                    break;
                }
            } catch (Exception error) {
                lastError = error;
            }
            try {
                Thread.sleep(100);
            } catch (InterruptedException error) {
                Thread.currentThread().interrupt();
                throw new IllegalStateException("interrupted while waiting for location status", error);
            }
        }
        if (status == null || !status.path("storeHealthy").asBoolean(false)) {
            throw new IllegalStateException(
                "consumer location runtime status did not become healthy; lastStatus="
                    + status + ", lastError=" + lastError);
        }
        ScenarioAssert.that(status.path("storeHealthy").asBoolean(false), "store status is not healthy");
        ScenarioAssert.that(status.path("ownerLeaseHealthy").asBoolean(false), "owner lease status is not healthy");
        return status;
    }

    public void waitForPollingOnlyStatus() {
        JsonNode status = Wait.until(
            Duration.ofSeconds(10),
            "consumer location runtime status did not report polling-only mode",
            () -> {
                JsonNode current = JSON.readTree(http.get("/locations/status"));
                boolean healthy = current.path("storeHealthy").asBoolean(false);
                boolean watchEnabled = current.path("watchEnabled").asBoolean(true);
                long pollingMillis = current.path("pollingIntervalMillis").asLong(0);
                return healthy && !watchEnabled && pollingMillis > 0 ? current : null;
            });
        ScenarioAssert.that(status.path("storeHealthy").asBoolean(false), "SF-A2 store status is not healthy");
        ScenarioAssert.that(!status.path("watchEnabled").asBoolean(true),
            "SF-A2 polling-only consumer unexpectedly reports watch enabled");
    }

    public void waitForUnhealthyStatus(String scenarioName) {
        JsonNode status = waitForStatus(
            Duration.ofMillis(options.locationHeartbeatMillis() * 8),
            current -> !current.path("storeHealthy").asBoolean(true)
                && !current.path("lastError").asText("").isBlank(),
            scenarioName + " did not record store outage");
        ScenarioAssert.that(!status.path("storeHealthy").asBoolean(true),
            scenarioName + " store status is not unhealthy");
    }

    public void waitForOwnerLeaseFailure(String scenarioName) {
        waitForStatus(
            Duration.ofMillis(options.locationHeartbeatMillis() * 8),
            current -> !current.path("ownerLeaseHealthy").asBoolean(true),
            scenarioName + " owner lease heartbeat failure did not surface");
    }

    public JsonNode waitForRecoveredStatus(String scenarioName) {
        return waitForStatus(
            Duration.ofMillis(options.locationHeartbeatMillis() * 10),
            current -> current.path("storeHealthy").asBoolean(false)
                && current.path("ownerLeaseHealthy").asBoolean(false),
            scenarioName + " status did not recover after store outage");
    }

    public JsonNode waitForStatus(
        Duration timeout,
        java.util.function.Predicate<JsonNode> accept,
        String failureMessage) {
        return Wait.until(
            timeout,
            failureMessage,
            () -> {
                try {
                    JsonNode current = JSON.readTree(http.get("/locations/status"));
                    return accept.test(current) ? current : null;
                } catch (Exception error) {
                    return null;
                }
            });
    }

    private static boolean containsAllRids(Set<String> observed, Set<String> expected) {
        for (String rid : expected) {
            if (observed.stream().noneMatch(value -> value.equals(rid) || value.contains(rid))) {
                return false;
            }
        }
        return true;
    }

    private static boolean isReadyPeer(JsonNode entry) {
        return "READY".equals(entry.path("state").asText(""))
            && !entry.path("draining").asBoolean(false);
    }

    @Override
    public void close() {
        http.close();
    }

}
