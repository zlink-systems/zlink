package systems.zlink.e2e.spotservice.client.Scenarios;

import java.io.IOException;
import java.net.URI;
import java.nio.file.Files;
import java.nio.file.Path;
import java.time.Duration;
import java.util.ArrayList;
import java.util.List;
import java.util.UUID;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import java.util.function.Supplier;
import systems.zlink.e2e.spotservice.shared.Contracts;
import systems.zlink.e2e.spotservice.client.ClientOptions;
import systems.zlink.e2e.spotservice.shared.ScenarioState;
import systems.zlink.httpclient.HttpResponse;
import systems.zlink.httpclient.RawHttpResponse;
import systems.zlink.httpclient.ZLinkHttpClient;
import systems.zlink.stream.connector.ZLinkStreamCompression;
import systems.zlink.stream.connector.ZLinkStreamConnector;
import systems.zlink.stream.connector.ZLinkStreamConnectorFactory;
import systems.zlink.stream.connector.ZLinkStreamConnectorOptions;
import systems.zlink.stream.connector.ZLinkStreamDispatchMode;
import systems.zlink.stream.connector.ZLinkStreamMessage;
import systems.zlink.stream.connector.ZLinkStreamPacketNameResolver;

public class SpotServiceScenarioContext {
    protected static final Duration REQUEST_TIMEOUT = Duration.ofSeconds(5);
    private static final Duration RELOCATION_TIMEOUT = Duration.ofSeconds(45);
    protected static final Duration EVENTUAL_TIMEOUT = Duration.ofSeconds(30);
    private final ClientOptions options;

    protected SpotServiceScenarioContext(SpotServiceScenarioContext source) {
        this(source.options);
    }

    public SpotServiceScenarioContext(ClientOptions options) {
        this.options = options;
    }

    protected ClientOptions options() { return options; }

    protected Contracts.StateRes requestState(String spotRid, String value, Duration timeout) {
        return postGateway("/operations/spot/state-request",
            new Contracts.SpotStateOperation(spotRid, value, timeout.toMillis()),
            Contracts.StateRes.class);
    }

    protected Contracts.StateRes requestSlow(String spotRid, String value, Duration timeout) {
        return postGateway("/operations/spot/slow-request",
            new Contracts.SpotStateOperation(spotRid, value, timeout.toMillis()),
            Contracts.StateRes.class);
    }

    protected Contracts.OutboundRes requestOutbound(String spotRid, String value) {
        return postGateway("/operations/spot/outbound-request",
            new Contracts.SpotValueOperation(spotRid, value),
            Contracts.OutboundRes.class);
    }

    protected void sendState(String spotRid, String value) {
        postGateway("/operations/spot/state-send",
            new Contracts.SpotValueOperation(spotRid, value),
            Contracts.OperationAccepted.class);
    }

    protected void sendOutbound(String spotRid, String value) {
        postGateway("/operations/spot/outbound-send",
            new Contracts.SpotValueOperation(spotRid, value),
            Contracts.OperationAccepted.class);
    }

    protected Contracts.RouteRes requestRoute(String nodeRid, String value) {
        return postGateway("/operations/route/request",
            new Contracts.RouteOperation(nodeRid, value, REQUEST_TIMEOUT.toMillis()),
            Contracts.RouteRes.class);
    }

    protected Contracts.ActorPingRes requestActorPush(String actorId, String value) {
        return postGateway("/operations/actor/push-request",
            new Contracts.ActorOperation(actorId, value, REQUEST_TIMEOUT.toMillis()),
            Contracts.ActorPingRes.class);
    }

    private <T> T postGateway(String path, Object request, Class<T> responseType) {
        return postJson(options.gatewayHttpEndpoint(), path, request, responseType);
    }

    protected void verifyActorJoinAdmission(
        String ownerEndpoint,
        String nodeRid,
        String spotRid,
        String actorId) {
        Contracts.ActorProfile allowedProfile =
            new Contracts.ActorProfile("Admission " + nodeRid, 9, List.of("admission", nodeRid));
        Contracts.ActorProfile rejectedProfile =
            new Contracts.ActorProfile("Rejected " + nodeRid, 9, List.of("admission", "rejected"));
        String rejectedActorId = actorId + "-rejected";
        postJson(
            ownerEndpoint,
            "/spot/create",
            new Contracts.CreateSpotReq(spotRid),
            Contracts.CreateSpotRes.class);

        ZLinkStreamConnector connector = createStreamConnector(options.streamAEndpoint());
        try {
            connector.connect().submit().toCompletableFuture().join();
            connector
                .request(new Contracts.ActorAuthReq(actorId, allowedProfile))
                .submit(Contracts.ActorAuthRes.class).toCompletableFuture().join();
            Contracts.JoinAdmittedUserSpotActorRes allowed = connector
                .request(new Contracts.JoinAdmittedUserSpotActorReq(
                    spotRid,
                    allowedProfile,
                    allowedProfile.tags(),
                    true,
                    "allowed"))
                .metadata("actor-id", actorId)
                .submit(Contracts.JoinAdmittedUserSpotActorRes.class).toCompletableFuture().join();
            ensure(allowed.accepted(), "SM-B9 allowed join was rejected");
            ensure(actorId.equals(allowed.actorId()), "SM-B9 allowed actor mismatch");
            ensure(spotRid.equals(allowed.spotRid()), "SM-B9 allowed spot mismatch");

            connector
                .request(new Contracts.ActorAuthReq(rejectedActorId, rejectedProfile))
                .metadata("actor-id", rejectedActorId)
                .submit(Contracts.ActorAuthRes.class).toCompletableFuture().join();
            Contracts.JoinAdmittedUserSpotActorRes rejected = connector
                .request(new Contracts.JoinAdmittedUserSpotActorReq(
                    spotRid,
                    rejectedProfile,
                    rejectedProfile.tags(),
                    false,
                    "capacity"))
                .metadata("actor-id", rejectedActorId)
                .submit(Contracts.JoinAdmittedUserSpotActorRes.class).toCompletableFuture().join();
            ensure(!rejected.accepted(), "SM-B9 rejected join was accepted");
            ensure("ActorJoinRejected".equals(rejected.errorKind()), "SM-B9 rejection was not classified");

            Contracts.EvidenceSnapshot evidence = waitForEvidence(
                ownerEndpoint,
                List.of(
                    "ActorUserJoinAdmitted|" + nodeRid + "|" + spotRid + "|" + actorId + "/allowed",
                    "ActorUserJoined|" + nodeRid + "|" + spotRid + "|" + actorId,
                    "ActorUserJoinRejected|" + nodeRid + "|" + spotRid + "|" + rejectedActorId + "/capacity"));
            long rejectedJoined = countActorEvidence(evidence, "ActorUserJoined", spotRid, rejectedActorId);
            ensure(rejectedJoined == 0, "SM-B9 rejected actor was joined to user spot");
        } catch (Exception error) {
            throw new IllegalStateException("actor join admission scenario failed for " + nodeRid, error);
        } finally {
            closeQuietly(connector);
        }
    }

    protected void setPlacementWeight(String endpoint, int weight) {
        postJson(
            endpoint,
            "/placement-weight",
            new Contracts.PlacementWeightReq(weight),
            Contracts.PlacementWeightRes.class);
    }

    protected Contracts.RelocationRes relocate(String endpoint) {
        return postJson(
            endpoint,
            "/admin/relocate",
            new Contracts.RelocationReq(),
            Contracts.RelocationRes.class,
            RELOCATION_TIMEOUT);
    }

    protected static long countEvidence(
        Contracts.EvidenceSnapshot evidence,
        String marker,
        String spotRid,
        String value) {
        return evidence.entries().stream()
            .filter(entry -> marker.equals(entry.marker())
                && spotRid.equals(entry.spotRid())
                && value.equals(entry.value()))
            .count();
    }

    protected static boolean containsSpotEvidence(
        Contracts.EvidenceSnapshot evidence,
        String spotRid) {
        return evidence.entries().stream()
            .anyMatch(entry -> spotRid.equals(entry.spotRid()));
    }

    protected static void authenticateJoinAndEcho(
        ZLinkStreamConnector connector,
        String actorId,
        Contracts.ActorProfile profile,
        String value,
        int requestSeq) {
        try {
            Contracts.ActorAuthRes auth = connector
                .request(new Contracts.ActorAuthReq(actorId, profile))
                .timeout(Duration.ofSeconds(15))
                .submit(Contracts.ActorAuthRes.class).toCompletableFuture().join();
            ensure(actorId.equals(auth.actorId()), "SM-G1 auth actor mismatch");

            Contracts.ActorJoinRes joined = connector
                .request(new Contracts.ActorJoinReq("room-a", profile, profile.tags()))
                .metadata("actor-id", actorId)
                .timeout(Duration.ofSeconds(15))
                .submit(Contracts.ActorJoinRes.class).toCompletableFuture().join();
            ensure("room-a".equals(joined.spotRid()), "SM-G1 joined spot mismatch");

            Contracts.ActorEchoRes echo = connector
                .request(new Contracts.ActorEchoReq(value, requestSeq, profile))
                .metadata("actor-id", actorId)
                .timeout(Duration.ofSeconds(15))
                .submit(Contracts.ActorEchoRes.class).toCompletableFuture().join();
            ensure("room-a".equals(echo.spotRid()), "SM-G1 actor spot mismatch");
            ensure(("user:" + value).equals(echo.value()), "SM-G1 actor echo mismatch");
        } catch (Exception error) {
            throw new IllegalStateException("SM-G1 auth/join/echo failed", error);
        }
    }

    protected static void signalFile(String path) {
        try {
            Files.writeString(Path.of(path), "ready\n");
        } catch (IOException error) {
            throw new IllegalStateException("failed to write signal file " + path, error);
        }
    }

    protected static void waitForSignalFile(String path) {
        Path signal = Path.of(path);
        long deadline = System.nanoTime() + Duration.ofSeconds(60).toNanos();
        while (System.nanoTime() < deadline) {
            if (Files.exists(signal)) {
                return;
            }
            try {
                Thread.sleep(100);
            } catch (InterruptedException error) {
                Thread.currentThread().interrupt();
                throw new IllegalStateException("interrupted while waiting for signal file " + path, error);
            }
        }
        throw new IllegalStateException("timed out waiting for signal file " + path);
    }

    protected static long countActorEvidence(
        Contracts.EvidenceSnapshot evidence,
        String marker,
        String spotRid,
        String actorId) {
        return evidence.entries().stream()
            .filter(entry -> marker.equals(entry.marker())
                && spotRid.equals(entry.spotRid())
                && entry.value().startsWith(actorId))
            .count();
    }

    protected static List<String> matchingActorEvidence(
        Contracts.EvidenceSnapshot evidence,
        String marker,
        String spotRid,
        String actorId) {
        return evidence.entries().stream()
            .filter(entry -> marker.equals(entry.marker())
                && spotRid.equals(entry.spotRid())
                && entry.value().startsWith(actorId))
            .<String>map(entry -> entry.marker()
                + "|" + entry.nodeRid()
                + "|" + entry.spotRid()
                + "|" + entry.value())
            .toList();
    }

    protected static <T> void awaitUnchecked(
        ZLinkStreamConnector connector,
        java.util.concurrent.CompletionStage<T> stage) {
        try {
            stage.toCompletableFuture().join();
        } catch (Exception error) {
            throw new RuntimeException(error);
        }
    }

    protected ZLinkStreamConnector createStreamConnector(String endpoint) {
        return createStreamConnector(endpoint, ZLinkStreamDispatchMode.IMMEDIATE, Integer.MAX_VALUE);
    }

    protected ZLinkStreamConnector createStreamConnector(
        String endpoint,
        ZLinkStreamDispatchMode dispatchMode,
        int maxReceivedMessages) {
        return createStreamConnector(endpoint, dispatchMode, maxReceivedMessages, false);
    }

    protected ZLinkStreamConnector createStreamConnector(
        String endpoint,
        ZLinkStreamDispatchMode dispatchMode,
        int maxReceivedMessages,
        boolean skipServerCertificateValidation) {
        return ZLinkStreamConnectorFactory.create(new ZLinkStreamConnectorOptions(
            URI.create(endpoint),
            dispatchMode,
            REQUEST_TIMEOUT,
            2,
            Duration.ofSeconds(5),
            64 * 1024,
            64 * 1024,
            maxReceivedMessages,
            true,
            Duration.ofSeconds(1),
            Duration.ofSeconds(5),
            false,
            Duration.ofMillis(250),
            Duration.ofSeconds(5),
            2.0,
            skipServerCertificateValidation,
            ZLinkStreamCompression.LZ4,
            ZLinkStreamPacketNameResolver.defaultResolver(),
            null));
    }

    protected static void closeQuietly(ZLinkStreamConnector connector) {
        try {
            connector.close().submit().toCompletableFuture().join();
        } catch (Exception ignored) {
        }
    }

    protected void closeSpot(String spotRid) {
        String endpoint = options().httpAEndpoint();
        try (ZLinkHttpClient http = ZLinkHttpClient.create(endpoint).build()) {
            RawHttpResponse response = http.post("/admin/close?rid=" + spotRid)
                .timeout(REQUEST_TIMEOUT)
                .submitRaw()
                .toCompletableFuture()
                .join();
            ensure(response.status() >= 200 && response.status() < 300,
                "spot close returned HTTP " + response.status());
            ensure(response.body().contains("\"closed\":true"), "spot close did not report closed=true");
        }
    }

    protected Contracts.EvidenceSnapshot waitForPlayAEvidence(List<String> fragments) {
        return postJson(
            options().httpAEndpoint(),
            "/evidence/wait",
            new Contracts.EvidenceWaitReq(fragments, 10_000),
            Contracts.EvidenceSnapshot.class);
    }

    protected Contracts.EvidenceSnapshot waitForPlayBEvidence(List<String> fragments) {
        return postJson(
            options().httpBEndpoint(),
            "/evidence/wait",
            new Contracts.EvidenceWaitReq(fragments, 10_000),
            Contracts.EvidenceSnapshot.class);
    }

    protected static Contracts.EvidenceSnapshot waitForEvidence(
        String endpoint,
        List<String> fragments) {
        return postJson(
            endpoint,
            "/evidence/wait",
            new Contracts.EvidenceWaitReq(fragments, 10_000),
            Contracts.EvidenceSnapshot.class);
    }

    protected static <T> T postJson(
        String endpoint,
        String path,
        Object request,
        Class<T> responseType) {
        return postJson(endpoint, path, request, responseType, REQUEST_TIMEOUT);
    }

    protected static <T> T postJson(
        String endpoint,
        String path,
        Object request,
        Class<T> responseType,
        Duration timeout) {
        try (ZLinkHttpClient http = ZLinkHttpClient.create(endpoint).build()) {
            HttpResponse<T> response = http.post(path)
                .timeout(timeout)
                .body(request)
                .submit(responseType)
                .toCompletableFuture()
                .join();
            if (response.status() < 200 || response.status() >= 300) {
                throw new IllegalStateException(
                    "evidence request failed with status "
                        + response.status());
            }
            return response.body();
        }
    }

    protected static void expectFailure(Runnable action) {
        try {
            action.run();
        } catch (RuntimeException error) {
            return;
        }
        throw new IllegalStateException("operation unexpectedly succeeded");
    }

    protected static <T> T eventually(Supplier<T> action) {
        long deadline = System.nanoTime() + EVENTUAL_TIMEOUT.toNanos();
        RuntimeException lastFailure = null;
        while (System.nanoTime() < deadline) {
            try {
                return action.get();
            } catch (RuntimeException error) {
                lastFailure = error;
                try {
                    Thread.sleep(200);
                } catch (InterruptedException interrupted) {
                    Thread.currentThread().interrupt();
                    throw new IllegalStateException("operation interrupted", interrupted);
                }
            }
        }
        throw new IllegalStateException("operation did not succeed before timeout", lastFailure);
    }

    protected static void sleep(long millis) {
        try {
            Thread.sleep(millis);
        } catch (InterruptedException error) {
            Thread.currentThread().interrupt();
            throw new IllegalStateException("operation interrupted", error);
        }
    }

    protected static void ensure(boolean condition, String message) {
        if (!condition) {
            throw new IllegalStateException(message);
        }
    }
}
