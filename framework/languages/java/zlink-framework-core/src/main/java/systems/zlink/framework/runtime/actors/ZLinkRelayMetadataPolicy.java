package systems.zlink.framework.runtime.actors;

import java.util.LinkedHashMap;
import java.util.Map;
import java.util.Set;
import systems.zlink.framework.runtime.streams.ZLinkStreamHeader;

final class ZLinkRelayMetadataPolicy {
    static final ZLinkRelayMetadataPolicy EMPTY =
        new ZLinkRelayMetadataPolicy(Set.of(), Set.of());

    private final Set<String> sessionToActorKeys;
    private final Set<String> actorToSessionKeys;

    ZLinkRelayMetadataPolicy(
        Set<String> sessionToActorKeys,
        Set<String> actorToSessionKeys) {
        this.sessionToActorKeys =
            sessionToActorKeys == null ? Set.of() : Set.copyOf(sessionToActorKeys);
        this.actorToSessionKeys =
            actorToSessionKeys == null ? Set.of() : Set.copyOf(actorToSessionKeys);
    }

    ZLinkStreamHeader sessionToActor(ZLinkStreamHeader header) {
        return copyWithMetadata(header, filter(header.metadata(), sessionToActorKeys));
    }

    ZLinkBoundSessionSendOptions actorToSession(ZLinkBoundSessionSendOptions options) {
        return new ZLinkBoundSessionSendOptions(
            options.defaultPacketName(),
            filter(options.metadata(), actorToSessionKeys),
            options.packetName(),
            options.codec());
    }

    private static ZLinkStreamHeader copyWithMetadata(
        ZLinkStreamHeader header,
        Map<String, String> metadata) {
        return new ZLinkStreamHeader(
            header.kind(),
            header.codec(),
            header.flags(),
            header.requestSequence(),
            header.name(),
            metadata,
            header.correlationId(),
            header.flowId(),
            header.flowOrigin());
    }

    private static Map<String, String> filter(
        Map<String, String> metadata,
        Set<String> allowedKeys) {
        if (metadata == null || metadata.isEmpty() || allowedKeys.isEmpty()) {
            return Map.of();
        }
        Map<String, String> filtered = new LinkedHashMap<>();
        metadata.forEach((key, value) -> {
            if (allowedKeys.contains(key)) {
                filtered.put(key, value);
            }
        });
        return Map.copyOf(filtered);
    }
}
