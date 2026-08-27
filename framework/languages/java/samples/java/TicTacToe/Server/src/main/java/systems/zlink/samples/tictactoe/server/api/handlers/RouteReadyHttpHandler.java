package systems.zlink.samples.tictactoe.server.api.handlers;

import org.springframework.http.ResponseEntity;
import org.springframework.web.bind.annotation.GetMapping;
import org.springframework.web.bind.annotation.RequestParam;
import org.springframework.web.bind.annotation.RestController;
import systems.zlink.contracts.core.RoutingId;
import systems.zlink.framework.monitoring.ZLinkPeerState;
import systems.zlink.framework.monitoring.ZLinkRouteMeshRuntime;
import systems.zlink.samples.tictactoe.server.configuration.SampleNames;

@RestController
public final class RouteReadyHttpHandler {
    private final ZLinkRouteMeshRuntime meshes;

    public RouteReadyHttpHandler(ZLinkRouteMeshRuntime meshes) {
        this.meshes = meshes;
    }

    @GetMapping("/ready")
    public ResponseEntity<Void> handle(@RequestParam("targetRid") String targetRid) {
        if (targetRid == null || targetRid.isBlank()) {
            return ResponseEntity.badRequest().build();
        }
        boolean ready = meshes.snapshot(SampleNames.SpotMesh).peers().stream()
            .anyMatch(peer -> peer.nodeRid().equals(RoutingId.from(targetRid))
                && peer.state() == ZLinkPeerState.READY);
        return ready
            ? ResponseEntity.ok().build()
            : ResponseEntity.status(503).build();
    }
}
