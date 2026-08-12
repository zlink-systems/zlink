package systems.zlink.e2e.kotlin.toactormessaging.shared;

import systems.zlink.framework.handlers.ZLinkPacket;

public final class Contracts {
    public static final String SPOT_MESH = "to-actor";
    public static final String ACTOR_TYPE = "test-actor";

    private Contracts() {
    }

    @ZLinkPacket("ActorMsg")
    public record ActorMsg(String scenario, String actorId, String value) {
    }

    @ZLinkPacket("ActorReq")
    public record ActorReq(String scenario, String actorId, String value) {
    }

    public record ActorRes(String scenario, String actorId, String value) {
    }

    public record ActorEvidence(String scenario, String actorId, String kind, String value) {
    }

    public record ActorCallReq(String scenario, String actorId, String value) {
    }

    public record ActorCreateReq(String reason) {
    }

    public record ActorFaultReq(String scenario, String actorId) {
    }

    public record ActorCallRes(
        String scenario,
        String actorId,
        String result,
        String errorKind) {
        public static ActorCallRes ok(String scenario, String actorId, String result) {
            return new ActorCallRes(scenario, actorId, result, null);
        }

        public static ActorCallRes failed(String scenario, String actorId, String kind) {
            return new ActorCallRes(scenario, actorId, "failed", kind);
        }
    }
}
