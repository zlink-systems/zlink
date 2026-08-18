package systems.zlink.crosslanguage.host;

/**
 * Cross-language spot route wire scenario DTOs. Packet names resolve from
 * the simple class name (ZLinkPacketNames.resolve), matching the type names
 * the C++/.NET/Node peer hosts already use for the same scenario: (a) an
 * echo request/reply, (b) a packet no host registers a handler for
 * (framework NOT_FOUND), (c) a request whose handler fails with a typed
 * application error kind.
 */
public final class SpotRouteContracts {
    private SpotRouteContracts() {
    }

    public record TestHostSpotRouteRequest(String value) {
    }

    public record TestHostSpotRouteReply(String value) {
    }

    public record TestHostSpotRouteFailRequest(String value) {
    }

    public record TestHostSpotRouteMissingRequest(String value) {
    }
}
