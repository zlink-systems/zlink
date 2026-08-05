package systems.zlink.e2e.storefailure.shared;

public final class Contracts {
    public static final String CHANNEL = "discovery.registry.ha.profile";
    public static final String HANDLER_GROUP = "store-failure";

    private Contracts() {
    }

    public record ProfileReq(
        String value,
        String marker) {
    }

    public record ProfileRes(
        String value,
        String providerRid,
        String marker) {
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
}
