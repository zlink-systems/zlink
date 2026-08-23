package systems.zlink.crosslanguage.host;

/**
 * DTOs for the cross-language User-Spot JoinSpot scenario (spec
 * 15-spot-actor.ko.md section 4.2 User Spot admission; spec
 * 51 section 9 canonical actorJoin command 28 as the Core request leg with
 * command 20 as its reply leg). Packet names resolve from the simple class
 * name, so these mirror the .NET TestHost records and the Node
 * user_spot_join_host.js classes one-for-one.
 */
public final class UserSpotJoinContracts {
    private UserSpotJoinContracts() {
    }

    /** Target-local: fixed User Spot creation payload. */
    public record UserSpotCreateReq(String marker) {
    }

    /** Source-local: entry-spot actor request that starts the deferred join. */
    public record BeginUserSpotJoinReq(String targetSpotId, String marker) {
    }

    /** Crosses the wire inside canonical command 28's application payload. */
    public record UserSpotJoinReq(String marker) {
    }

    /** Admission reply; travels back on the command-20 reply leg. */
    public record UserSpotJoinRes(
        boolean accepted, String actorId, String spotId, String nodeRid, String marker) {
    }

    public record UserSpotProbeReq(String marker) {
    }

    public record UserSpotProbeRes(
        String actorId, String spotId, String nodeRid, int stateVersion, String marker) {
    }

    /** Reciprocal-discovery probe the target sends to the source node. */
    public record UserSpotDiscoveryProbeReq(String marker) {
    }

    public record UserSpotDiscoveryProbeRes(String marker, String nodeRid) {
    }
}
