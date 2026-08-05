package systems.zlink.framework.runtime.locations;

import java.util.HashMap;
import java.util.List;
import java.util.Map;
import systems.zlink.contracts.core.RoutingId;
import systems.zlink.framework.runtime.internal.locations.ZLinkAutoConnectType;
import systems.zlink.framework.locations.ZLinkLocationRole;
import systems.zlink.framework.locations.ZLinkMeshNodeObjectRole;
import systems.zlink.framework.runtime.internal.locations.ZLinkAutoConnectPeer;

final class ZLinkAutoConnectPlanner {
    static final String SECURITY_IDENTITY_METADATA_KEY =
        "zlink.security-identity";

    private ZLinkAutoConnectPlanner() {
    }

    record Local(
        ZLinkAutoConnectType type,
        String meshName,
        ZLinkLocationRole role,
        RoutingId nodeRid,
        String endpoint,
        ZLinkMeshNodeObjectRole objectRole,
        boolean hasRouteMeshServerChannel) {

        Local(
            ZLinkAutoConnectType type,
            String meshName,
            ZLinkLocationRole role,
            RoutingId nodeRid,
            String endpoint) {
            this(
                type,
                meshName,
                role,
                nodeRid,
                endpoint,
                ZLinkMeshNodeObjectRole.NONE,
                false);
        }
    }

    record Target(
        String key,
        RoutingId nodeRid,
        ZLinkLocationRole role,
        String endpoint,
        Map<String, String> metadata,
        String ownerId,
        long lifecycleGeneration) {
    }

    record PeerDecision(
        ZLinkAutoConnectPeer peer,
        Target target,
        String skipReason) {

        boolean shouldDial() {
            return target != null;
        }
    }

    static boolean isRoleAllowed(
        ZLinkAutoConnectType type,
        ZLinkLocationRole role) {
        return switch (type) {
            case ROUTE_MESH -> role == ZLinkLocationRole.ROUTER;
            case CLIENT_SERVER -> role == ZLinkLocationRole.ROUTER
                || role == ZLinkLocationRole.DEALER;
            case DEALER_MESH -> role == ZLinkLocationRole.DEALER;
            case FANOUT -> role == ZLinkLocationRole.PUB || role == ZLinkLocationRole.SUB;
            case SPOT_MESH -> role == ZLinkLocationRole.SPOT
                || role == ZLinkLocationRole.ROUTER;
            default -> false;
        };
    }

    static Map<String, Target> computeDesired(Local local, List<ZLinkAutoConnectPeer> peers) {
        Map<String, Target> desired = new HashMap<>();
        for (ZLinkAutoConnectPeer peer : peers) {
            PeerDecision decision = decide(local, peer);
            if (decision.shouldDial()) {
                desired.put(decision.target().key(), decision.target());
            }
        }
        return desired;
    }

    static Map<String, Target> computeNotRequired(
        Local local,
        List<ZLinkAutoConnectPeer> peers) {
        Map<String, Target> notRequired = new HashMap<>();
        if (local.type() != ZLinkAutoConnectType.ROUTE_MESH) {
            return notRequired;
        }
        for (ZLinkAutoConnectPeer peer : peers) {
            PeerDecision validated = validate(local, peer);
            if (validated.skipReason() == null
                && isRouteMeshConnectionNotRequired(local, peer)) {
                Target target = targetOf(peer);
                notRequired.put(target.key(), target);
            }
        }
        return notRequired;
    }

    static Target trackableTarget(Local local, ZLinkAutoConnectPeer peer) {
        PeerDecision decision = validate(local, peer);
        return decision.skipReason() == null ? targetOf(peer) : null;
    }

    static List<PeerDecision> decideAll(Local local, List<ZLinkAutoConnectPeer> peers) {
        return peers.stream().map(peer -> decide(local, peer)).toList();
    }

    private static PeerDecision decide(Local local, ZLinkAutoConnectPeer peer) {
        PeerDecision validated = validate(local, peer);
        if (validated.skipReason() != null) {
            return validated;
        }
        if (!shouldDial(local, peer)) {
            return skip(peer, "not-initiator");
        }
        return new PeerDecision(peer, targetOf(peer), null);
    }

    private static PeerDecision validate(Local local, ZLinkAutoConnectPeer peer) {
        if (peer.autoConnectType() != local.type()) {
            return skip(peer, "type-mismatch");
        }
        if (!peer.meshName().equals(local.meshName())) {
            return skip(peer, "mesh-mismatch");
        }
        if (!isRoleAllowed(local.type(), peer.role())) {
            return skip(peer, "role-not-allowed");
        }
        if (peer.endpoint() == null || peer.endpoint().isBlank()) {
            return skip(peer, "missing-endpoint");
        }
        if (isSelf(local, peer)) {
            return skip(peer, "self");
        }
        return new PeerDecision(peer, null, null);
    }

    private static Target targetOf(ZLinkAutoConnectPeer peer) {
        return new Target(
            targetKeyOf(peer),
            peer.nodeRid(),
            peer.role(),
            peer.endpoint(),
            peer.metadata(),
            peer.ownerId(),
            lifecycleGenerationOf(peer));
    }

    private static PeerDecision skip(ZLinkAutoConnectPeer peer, String reason) {
        return new PeerDecision(peer, null, reason);
    }

    private static String targetKeyOf(ZLinkAutoConnectPeer peer) {
        String identity = hasRid(peer.nodeRid())
            ? peer.nodeRid().toHex()
            : peer.endpoint();
        return peer.role().name().toLowerCase(java.util.Locale.ROOT)
            + "|"
            + identity
            + "|"
            + lifecycleGenerationOf(peer);
    }

    private static long lifecycleGenerationOf(ZLinkAutoConnectPeer peer) {
        return peer.generation();
    }

    private static boolean isSelf(Local local, ZLinkAutoConnectPeer peer) {
        if (hasRid(local.nodeRid()) && hasRid(peer.nodeRid())
            && local.nodeRid().equals(peer.nodeRid())) {
            return true;
        }
        return peer.endpoint().equals(local.endpoint());
    }

    private static boolean shouldDial(Local local, ZLinkAutoConnectPeer peer) {
        return switch (local.type()) {
            case ROUTE_MESH -> local.role() == ZLinkLocationRole.ROUTER
                && peer.role() == ZLinkLocationRole.ROUTER
                && !isRouteMeshConnectionNotRequired(local, peer)
                && localIsInitiator(local, peer);
            case CLIENT_SERVER -> local.role() == ZLinkLocationRole.DEALER
                && peer.role() == ZLinkLocationRole.ROUTER;
            case DEALER_MESH -> local.role() == ZLinkLocationRole.DEALER
                && peer.role() == ZLinkLocationRole.DEALER
                && localIsInitiator(local, peer);
            case FANOUT -> local.role() == ZLinkLocationRole.SUB
                && peer.role() == ZLinkLocationRole.PUB;
            case SPOT_MESH -> local.role() == ZLinkLocationRole.SPOT
                && peer.role() == ZLinkLocationRole.SPOT;
            default -> false;
        };
    }

    static boolean isRouteMeshConnectionNotRequired(
        Local local,
        ZLinkAutoConnectPeer peer) {
        return local.objectRole() == ZLinkMeshNodeObjectRole.CLIENT
            && !local.hasRouteMeshServerChannel()
            && peer.objectRole() == ZLinkMeshNodeObjectRole.CLIENT
            && !peer.hasRouteMeshServerChannel();
    }

    private static boolean localIsInitiator(Local local, ZLinkAutoConnectPeer peer) {
        if (local.endpoint() == null || local.endpoint().isBlank()) {
            return true;
        }
        if (hasRid(local.nodeRid()) && hasRid(peer.nodeRid())) {
            int byRid = local.nodeRid().toHex().compareTo(peer.nodeRid().toHex());
            if (byRid != 0) {
                return byRid < 0;
            }
        }
        return local.endpoint().compareTo(peer.endpoint()) < 0;
    }

    static boolean hasRid(RoutingId rid) {
        return rid != null && rid.size() > 0;
    }
}
