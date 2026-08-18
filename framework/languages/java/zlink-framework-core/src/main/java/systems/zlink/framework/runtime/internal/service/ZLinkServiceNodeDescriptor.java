package systems.zlink.framework.runtime.internal.service;

import java.util.Comparator;
import java.util.List;
import java.util.Objects;
import systems.zlink.contracts.core.RoutingId;
import systems.zlink.framework.runtime.internal.transport.ZLinkEndpointNotation;

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
    long applicationVersion,
    List<String> protocolCapabilities,
    ObjectRole objectRole,
    int placementWeight,
    int activeCapacityLimit,
    int pendingCapacityLimit,
    int activeCapacityUsed,
    int pendingCapacityUsed) {
    public static final String REQUIRED_CAPABILITY = "framework-service-v12";

    /**
     * Shared admission identity of the plaintext ROUTER transport. The
     * MeshNode socket exposes no authenticated peer identity, so every
     * language encodes this single placeholder in {@code securityIdentity}
     * (C++ {@code service_topology_registry.hpp} default, Node
     * {@code node-raw-mesh-backend.createDescriptor}, .NET
     * {@code ZLinkServiceSecurityIdentity.Plaintext}) and compares an
     * expectation against it. It is NOT a routing id.
     */
    public static final String PLAINTEXT_SECURITY_IDENTITY = "default";

    public ZLinkServiceNodeDescriptor {
        meshName = requireText(meshName, "meshName");
        Objects.requireNonNull(nodeRoutingId, "nodeRoutingId");
        //  Write-time normalization (endpoint notation policy §2.3): both
        //  the local RouteMesh descriptor and every peer descriptor
        //  decoded off the wire (ZLinkServiceM6AWireCodec) construct this
        //  record, so normalizing here keeps admission/dedup comparisons
        //  (e.g. ZLinkJavaRawMeshNode's peerIntents endpoint matching)
        //  notation-insensitive without touching each comparison site.
        advertisedEndpoint = ZLinkEndpointNotation.normalize(
            requireText(advertisedEndpoint, "advertisedEndpoint"));
        securityIdentity = requireText(securityIdentity, "securityIdentity");
        Objects.requireNonNull(state, "state");
        Objects.requireNonNull(objectRole, "objectRole");
        //  The lifecycle generation is an opaque CSPRNG equality token over
        //  the full unsigned 64-bit range (spec 13 §7.1: "which lifecycle is
        //  newer isn't judged by numeric magnitude"), so any non-zero bit
        //  pattern is valid here -- including the ~50% of peer-generated
        //  tokens whose bit 63 is set and that read back as a negative long.
        //  Only the descriptor revision is ordered, so only it is bounded to
        //  the range where signed comparison is the unsigned comparison.
        if (lifecycleGeneration == 0) {
            throw new IllegalArgumentException(
                "lifecycle generation must be non-zero");
        }
        if (descriptorRevision <= 0) {
            throw new IllegalArgumentException(
                "descriptor revision must be positive");
        }
        if (applicationVersion < 0) {
            throw new IllegalArgumentException(
                "application version is invalid");
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
