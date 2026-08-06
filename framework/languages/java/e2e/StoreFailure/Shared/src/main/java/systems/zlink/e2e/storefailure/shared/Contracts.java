package systems.zlink.e2e.storefailure.shared;

public final class Contracts {
    public static final String CHANNEL = "discovery.registry.ha.profile";
    public static final String HANDLER_GROUP = "store-failure";
    public static final String LEASE_PROBE_SPOT_TYPE = "store-failure-lease-probe";
    public static final String C4_ROUTE_A_MESH = "store-failure-c4-route-a";
    public static final String C4_ROUTE_B_MESH = "store-failure-c4-route-b";
    public static final String C4_ROUTE_A_CHANNEL = "store-failure-c4-route-a-channel";
    public static final String C4_ROUTE_B_CHANNEL = "store-failure-c4-route-b-channel";
    public static final String C4_CLIENT_SERVER_CHANNEL = "store-failure-c4-client-server";
    public static final String C4_FANOUT_CHANNEL = "store-failure-c4-fanout";
    public static final String C4_FANOUT_TOPIC = "store-failure-c4-topic";

    private Contracts() {
    }

    public record ProfileReq(
        String value,
        String marker) {
    }

    public record ProfileRes(
        String value,
        String providerRid,
        String providerLifecycle,
        String marker) {
    }

    public record InstanceReq(
        String spotId,
        String marker) {
    }

    public record InstanceRes(
        String spotId,
        String ownerRid,
        String ownerLifecycle,
        long objectGeneration,
        String marker) {
    }

    public record InstanceOutcome(
        boolean succeeded,
        InstanceRes reply,
        String errorKind,
        String errorMessage) {
    }

    public record EvidenceWaitReq(
        String contains,
        int timeoutMilliseconds) {
    }

    public record TopologyReadyWaitReq(
        int readyCount,
        int timeoutMilliseconds) {
    }

    public record MemberEndpointWaitReq(
        String endpoint,
        int timeoutMilliseconds) {
    }

    public record StoreDelayReq(
        int delayMilliseconds) {
    }

    public record ObjectLocationQueryReq(
        int pageSize,
        String continuationToken) {
    }

    public record C4ProbeReq(
        String marker) {
    }

    public record C4ProbeRes(
        String role,
        String providerRid,
        String providerLifecycle,
        String marker) {
    }

    public record C4RolesRes(
        String routeA,
        String routeALifecycle,
        String routeB,
        String routeBLifecycle,
        String clientServer,
        String clientServerLifecycle,
        String marker) {
    }

    public record C4FanoutEvent(
        String providerRid,
        String providerLifecycle,
        String marker) {
    }
}
