package systems.zlink.e2e.registrymessaging.objectclient;

import java.time.Duration;
import java.util.LinkedHashMap;
import java.util.Map;
import java.util.concurrent.CompletionStage;
import org.springframework.web.bind.annotation.GetMapping;
import org.springframework.web.bind.annotation.PostMapping;
import org.springframework.web.bind.annotation.RequestBody;
import org.springframework.web.bind.annotation.RestController;
import systems.zlink.contracts.core.RoutingId;
import systems.zlink.e2e.registrymessaging.shared.Contracts;
import systems.zlink.framework.channels.ZLinkRouteClient;
import systems.zlink.framework.errors.ZLinkFrameworkErrorKind;
import systems.zlink.framework.errors.ZLinkFrameworkException;
import systems.zlink.framework.monitoring.ZLinkRouteMeshRuntime;
import systems.zlink.framework.monitoring.ZLinkPeerState;

@RestController
public final class ObjectClientEndpoints {
    private final ZLinkRouteClient routes;
    private final ZLinkRouteMeshRuntime runtime;
    private final ObjectClientOptions options;

    public ObjectClientEndpoints(
        ZLinkRouteClient routes,
        ZLinkRouteMeshRuntime runtime,
        ObjectClientOptions options) {
        this.routes = routes;
        this.runtime = runtime;
        this.options = options;
    }

    @GetMapping("/health")
    public Map<String, String> health() {
        return Map.of("status", "ready");
    }

    @GetMapping("/rm-a3/status")
    public Map<String, Object> status() {
        var snapshot = runtime.snapshot(Program.meshName());
        var peers = snapshot.peers().stream()
            .map(peer -> {
                Map<String, Object> value = new LinkedHashMap<>();
                value.put("rid", peer.nodeRid().toString());
                value.put("state", peer.state().name().toLowerCase(java.util.Locale.ROOT));
                value.put("ready", peer.state() == ZLinkPeerState.READY);
                value.put(
                    "lastFailure",
                    peer.unavailableReason()
                        .map(reason -> reason.name().toLowerCase(java.util.Locale.ROOT))
                        .orElse(""));
                return value;
            })
            .toList();
        return Map.of(
            "rid", options.clientRid(),
            "readyPeerCount", peers.stream()
                .filter(peer -> Boolean.TRUE.equals(peer.get("ready")))
                .count(),
            "peers", peers);
    }

    @PostMapping("/rm-a3/node-direct")
    public CompletionStage<Map<String, String>> nodeDirect(
        @RequestBody NodeDirectRequest request) {
        RoutingId target = RoutingId.from(request.targetRid());
        var send = routes.sendToNode(
                Program.meshName(),
                target,
                new Contracts.RouteReq("rm-a3-send"))
            .submit()
            .handle((ignored, failure) -> errorKind(failure));
        var requestCall = routes.requestToNode(
                Program.meshName(),
                target,
                new Contracts.RouteReq("rm-a3-request"))
            .timeout(Duration.ofSeconds(2))
            .submit(Contracts.RouteRes.class)
            .handle((ignored, failure) -> errorKind(failure));
        return send.thenCombine(requestCall, (sendKind, requestKind) -> Map.of(
            "terminal", sendKind == ZLinkFrameworkErrorKind.NOT_FOUND
                && requestKind == ZLinkFrameworkErrorKind.NOT_FOUND
                    ? "NotFound"
                    : "Unexpected",
            "sendErrorKind", sendKind.name(),
            "requestErrorKind", requestKind.name()));
    }

    private static ZLinkFrameworkErrorKind errorKind(Throwable failure) {
        Throwable current = failure;
        while (current instanceof java.util.concurrent.CompletionException
            && current.getCause() != null) {
            current = current.getCause();
        }
        if (current instanceof ZLinkFrameworkException frameworkFailure) {
            return frameworkFailure.kind();
        }
        throw new IllegalStateException(
            "expected typed Framework failure but received " + current,
            current);
    }

    public record NodeDirectRequest(String targetRid) {
        public NodeDirectRequest {
            if (targetRid == null || targetRid.isBlank()) {
                throw new IllegalArgumentException("targetRid is required");
            }
        }
    }
}
