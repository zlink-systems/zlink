package systems.zlink.e2e.kotlin.toactormessaging.shared;

import systems.zlink.framework.handlers.ZLinkPacket;

public final class Contracts {
    public static final String SPOT_MESH = "to-actor";
    public static final String ACTOR_TYPE = "test-actor";

    private Contracts() {
    }

    @ZLinkPacket("ActorNotify")
    public record ActorNotify(String scenario, String actorId, String value) {
    }

    @ZLinkPacket("ActorAsk")
    public record ActorAsk(String scenario, String actorId, String value) {
    }

    public record ActorReply(String scenario, String actorId, String value) {
    }

    public record ActorEvidence(String scenario, String actorId, String kind, String value) {
    }

    public record ActorCallRequest(String scenario, String actorId, String value) {
    }

    public record ActorFaultRequest(String scenario, String actorId) {
    }

    public record ActorCallResponse(
        String scenario,
        String actorId,
        String result,
        String errorKind) {
        public static ActorCallResponse ok(String scenario, String actorId, String result) {
            return new ActorCallResponse(scenario, actorId, result, null);
        }

        public static ActorCallResponse failed(String scenario, String actorId, String kind) {
            return new ActorCallResponse(scenario, actorId, "failed", kind);
        }
    }
}
