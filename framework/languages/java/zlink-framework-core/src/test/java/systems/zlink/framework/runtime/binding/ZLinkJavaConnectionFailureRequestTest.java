package systems.zlink.framework.runtime.binding;

import static org.junit.jupiter.api.Assertions.*;

import org.junit.jupiter.api.Test;

import systems.zlink.contracts.core.RoutingId;
import systems.zlink.contracts.core.Zlink;
import systems.zlink.framework.errors.ZLinkFrameworkErrorKind;
import systems.zlink.framework.errors.ZLinkFrameworkException;
import systems.zlink.framework.locations.ZLinkMeshNodeObjectRole;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendActorRef;
import systems.zlink.framework.runtime.internal.backend.ZLinkInternalMeshNode;
import systems.zlink.framework.runtime.internal.service.ZLinkServiceNodeDescriptor;

import java.net.ServerSocket;
import java.time.Duration;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.TimeUnit;

final class ZLinkJavaConnectionFailureRequestTest {
    @Test
    void actualConnectionRefusalTerminatesJoinBeforeItsOriginalDeadline() throws Exception {
        try (var listener = new ServerSocket(0);
                var context = Zlink.createContext();
                var source = new ZLinkJavaRawMeshNode(context, "failed-owner")) {
            var endpoint = "tcp://127.0.0.1:" + listener.getLocalPort();
            var owner = RoutingId.from("closed-owner");
            source.setRoutingId(RoutingId.from("join-source"));
            source.setBind("tcp://127.0.0.1:0");
            source.start();
            source.observePeerAdmissionExpectation(
                            owner,
                            endpoint,
                            1,
                            ZLinkServiceNodeDescriptor.PLAINTEXT_SECURITY_IDENTITY,
                            ZLinkMeshNodeObjectRole.SERVER)
                    .toCompletableFuture()
                    .join();
            // Release the refusal target only after the source has bound its ephemeral port.
            listener.close();
            source.connectPeer(endpoint, owner);
            var join =
                    new ZLinkInternalMeshNode.CanonicalActorJoinRequest(
                            new ZLinkBackendActorRef(source.routingId(), "actor", 1),
                            source.lifecycleGeneration(),
                            1,
                            1,
                            owner,
                            1,
                            "remote-spot",
                            1,
                            1,
                            1,
                            false,
                            "probe",
                            "application/json",
                            "{}".getBytes(java.nio.charset.StandardCharsets.UTF_8));
            var request =
                    source.requestCanonicalActorJoin(join, Duration.ofSeconds(30))
                            .toCompletableFuture();
            var observed = request.handle((reply, failure) -> failure).toCompletableFuture();
            var checkpoint = new CompletableFuture<Throwable>();
            CompletableFuture.delayedExecutor(5, TimeUnit.SECONDS)
                    .execute(() -> checkpoint.complete(null));
            Throwable failure =
                    (Throwable)
                            CompletableFuture.anyOf(observed, checkpoint).get(6, TimeUnit.SECONDS);
            assertNotNull(
                    failure,
                    () ->
                            "Core connection refusal must end Join before its 30s deadline"
                                    + " (target="
                                    + endpoint
                                    + ", source="
                                    + source.status().localEndpoint()
                                    + ")");
            while (failure instanceof java.util.concurrent.CompletionException
                    && failure.getCause() != null) failure = failure.getCause();
            assertEquals(
                    ZLinkFrameworkErrorKind.UNAVAILABLE,
                    assertInstanceOf(ZLinkFrameworkException.class, failure).kind());
        }
    }
}
