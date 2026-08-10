package systems.zlink.framework.runtime.actors;

import java.util.Objects;
import java.util.Optional;
import java.util.concurrent.ConcurrentHashMap;
import java.util.concurrent.ConcurrentMap;
import java.util.concurrent.atomic.AtomicLong;
import systems.zlink.framework.runtime.internal.service.ZLinkServiceMessageFollowWireCodec;

/** Suppresses Message Follow notices for one exact source/target route fence. */
final class ZLinkMessageFollowSuppressionRegistry {
    private final ConcurrentMap<Key, Marker> markers = new ConcurrentHashMap<>();
    private final AtomicLong claims = new AtomicLong();

    Optional<Claim> begin(Key key) {
        Objects.requireNonNull(key, "key");
        Claim[] granted = new Claim[1];
        markers.compute(key, (ignored, current) -> {
            if (current != null && current.state() != State.IDLE) {
                return current;
            }
            Claim claim = new Claim(key, claims.incrementAndGet());
            granted[0] = claim;
            return new Marker(State.IN_FLIGHT, claim.token());
        });
        return Optional.ofNullable(granted[0]);
    }

    boolean markSent(Claim claim) {
        return complete(claim, State.SENT_UNTIL_EXPIRY);
    }

    boolean abort(Claim claim) {
        return complete(claim, State.IDLE);
    }

    private boolean complete(Claim claim, State target) {
        Objects.requireNonNull(claim, "claim");
        boolean[] applied = new boolean[1];
        markers.computeIfPresent(claim.key(), (ignored, current) -> {
            if (current.state() != State.IN_FLIGHT
                || current.claimToken() != claim.token()) {
                return current;
            }
            applied[0] = true;
            return new Marker(target, 0);
        });
        return applied[0];
    }

    boolean expire(Key key) {
        return markers.remove(Objects.requireNonNull(key, "key")) != null;
    }

    State state(Key key) {
        Marker marker = markers.get(key);
        return marker == null ? State.IDLE : marker.state();
    }

    int size() {
        return markers.size();
    }

    enum State {
        IDLE,
        IN_FLIGHT,
        SENT_UNTIL_EXPIRY
    }

    record Claim(Key key, long token) {
        Claim {
            Objects.requireNonNull(key, "key");
            if (token <= 0) {
                throw new IllegalArgumentException("claim token must be positive");
            }
        }
    }

    record Key(RouteFence sourceRoute, RouteFence targetRoute) {
        Key {
            Objects.requireNonNull(sourceRoute, "sourceRoute");
            Objects.requireNonNull(targetRoute, "targetRoute");
        }

        static Key actor(
            ZLinkServiceMessageFollowWireCodec.ActorRoute sourceRoute,
            ZLinkServiceMessageFollowWireCodec.ActorRoute targetRoute) {
            return new Key(RouteFence.actor(sourceRoute), RouteFence.actor(targetRoute));
        }
    }

    record RouteFence(
        String objectKind,
        String logicalObjectId,
        long objectGeneration,
        String targetNodeRid,
        long targetNodeGeneration,
        long authorityOwnerGeneration,
        long ownerLeaseGeneration) {
        RouteFence {
            if (!"actor".equals(objectKind)) {
                throw new IllegalArgumentException("Message Follow object kind must be actor");
            }
            Objects.requireNonNull(logicalObjectId, "logicalObjectId");
            Objects.requireNonNull(targetNodeRid, "targetNodeRid");
        }

        static RouteFence actor(ZLinkServiceMessageFollowWireCodec.ActorRoute route) {
            Objects.requireNonNull(route, "route");
            return new RouteFence(
                "actor",
                route.actorId(),
                route.objectGeneration(),
                route.targetNodeRid().toString(),
                route.targetNodeGeneration(),
                route.authorityOwnerGeneration(),
                route.ownerLeaseGeneration());
        }
    }

    private record Marker(State state, long claimToken) {
    }
}
