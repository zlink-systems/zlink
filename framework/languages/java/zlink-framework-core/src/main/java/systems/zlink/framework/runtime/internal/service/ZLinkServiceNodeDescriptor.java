package systems.zlink.framework.runtime.internal.service;

import java.util.Comparator;
import java.util.List;
import java.util.Objects;
import systems.zlink.contracts.core.RoutingId;

/** Immutable descriptor owned by the Framework RouteMesh runtime. */
public record ZLinkServiceNodeDescriptor(
    String meshName,
    RoutingId nodeRoutingId,
    long lifecycleGeneration,
    long descriptorRevision,
    String advertisedEndpoint,
    List<Channel> channels,
    State state,
    String securityIdentity,
    int effectiveMaxMessageBytes,
    long applicationVersion,
    List<String> protocolCapabilities,
    ObjectRole objectRole,
    int placementWeight,
    int activeCapacityLimit,
    int pendingCapacityLimit,
    int activeCapacityUsed,
    int pendingCapacityUsed) {
    public static final String REQUIRED_CAPABILITY = "framework-service-v11";

    public ZLinkServiceNodeDescriptor {
        meshName = requireText(meshName, "meshName");
        Objects.requireNonNull(nodeRoutingId, "nodeRoutingId");
        advertisedEndpoint = requireText(advertisedEndpoint, "advertisedEndpoint");
        securityIdentity = requireText(securityIdentity, "securityIdentity");
        Objects.requireNonNull(state, "state");
        Objects.requireNonNull(objectRole, "objectRole");
        if (lifecycleGeneration <= 0 || descriptorRevision <= 0) {
            throw new IllegalArgumentException(
                "lifecycle generation and descriptor revision must be positive");
        }
        if (effectiveMaxMessageBytes <= 0 || applicationVersion < 0) {
            throw new IllegalArgumentException(
                "effective message bound or application version is invalid");
        }
        validateWeight(placementWeight, "placementWeight");
        validateCapacity(activeCapacityLimit, false, "activeCapacityLimit");
        validateCapacity(pendingCapacityLimit, true, "pendingCapacityLimit");
        validateCapacity(activeCapacityUsed, true, "activeCapacityUsed");
        validateCapacity(pendingCapacityUsed, true, "pendingCapacityUsed");
        if (activeCapacityUsed > activeCapacityLimit
            || pendingCapacityUsed > pendingCapacityLimit) {
            throw new IllegalArgumentException("capacity use exceeds its limit");
        }

        channels = List.copyOf(Objects.requireNonNull(channels, "channels"));
        for (Channel channel : channels) {
            Objects.requireNonNull(channel, "channel");
        }
        requireSortedUnique(
            channels.stream().map(Channel::name).toList(),
            "channels");
        protocolCapabilities =
            List.copyOf(Objects.requireNonNull(protocolCapabilities, "protocolCapabilities"));
        requireSortedUnique(protocolCapabilities, "protocolCapabilities");
        if (!protocolCapabilities.contains(REQUIRED_CAPABILITY)) {
            throw new IllegalArgumentException(
                "protocol capability is required: " + REQUIRED_CAPABILITY);
        }
    }

    public boolean serves(String channelName) {
        return state == State.SERVING
            && channels.stream().anyMatch(
                channel -> channel.name().equals(channelName) && channel.weight() > 0);
    }

    public boolean acceptsPlacement() {
        return state == State.SERVING
            && objectRole == ObjectRole.SERVER
            && placementWeight > 0
            && activeCapacityUsed < activeCapacityLimit
            && pendingCapacityUsed < pendingCapacityLimit;
    }

    public record Channel(String name, int weight) {
        public Channel {
            name = requireText(name, "channel.name");
            validateWeight(weight, "channel.weight");
        }
    }

    public enum State {
        PREPARING,
        SERVING,
        RETIRING,
        DRAINING,
        STOPPED,
        ERROR
    }

    public enum ObjectRole {
        NONE,
        CLIENT,
        SERVER
    }

    private static void requireSortedUnique(List<String> values, String field) {
        String previous = null;
        for (String value : values) {
            String current = requireText(value, field);
            if (previous != null && Comparator.<String>naturalOrder().compare(previous, current) >= 0) {
                throw new IllegalArgumentException(field + " must be sorted and unique");
            }
            previous = current;
        }
    }

    private static String requireText(String value, String field) {
        if (value == null || value.isEmpty() || value.indexOf('\0') >= 0) {
            throw new IllegalArgumentException(
                field + " must be non-empty text without NUL");
        }
        return value;
    }

    private static void validateWeight(int value, String field) {
        if (value < 0 || value > 10_000) {
            throw new IllegalArgumentException(
                field + " must be in the range 0..10000");
        }
    }

    private static void validateCapacity(int value, boolean allowZero, String field) {
        if (value < (allowZero ? 0 : 1)) {
            throw new IllegalArgumentException(field + " is outside its supported range");
        }
    }
}
