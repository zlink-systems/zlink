package systems.zlink.framework.actors;

import java.nio.charset.StandardCharsets;
import java.util.Objects;
import systems.zlink.contracts.core.RoutingId;

public record ActorRef(
    String actorId,
    long objectGeneration,
    String meshName,
    RoutingId nodeRid) {
    public ActorRef {
        Objects.requireNonNull(actorId, "actorId");
        int byteLength = actorId.getBytes(StandardCharsets.UTF_8).length;
        if (byteLength < 1 || byteLength > 255) {
            throw new IllegalArgumentException(
                "actorId must contain 1..255 UTF-8 bytes");
        }
        if (objectGeneration <= 0) {
            throw new IllegalArgumentException(
                "objectGeneration must be positive");
        }
        if (meshName == null || meshName.isBlank()) {
            throw new IllegalArgumentException("meshName must not be blank");
        }
        Objects.requireNonNull(nodeRid, "nodeRid");
    }
}
