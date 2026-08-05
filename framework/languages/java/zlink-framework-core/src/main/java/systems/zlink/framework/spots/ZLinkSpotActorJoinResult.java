package systems.zlink.framework.spots;

import systems.zlink.framework.messaging.ZLinkMessage;

public record ZLinkSpotActorJoinResult(
    boolean accepted,
    ZLinkMessage reply) {
    public static ZLinkSpotActorJoinResult accept() {
        return from(ZLinkSpotAcceptRejectResult.accept());
    }

    public static ZLinkSpotActorJoinResult accept(ZLinkMessage reply) {
        return from(ZLinkSpotAcceptRejectResult.accept(reply));
    }

    public static ZLinkSpotActorJoinResult accept(Object reply) {
        return from(ZLinkSpotAcceptRejectResult.accept(reply));
    }

    public static ZLinkSpotActorJoinResult reject() {
        return from(ZLinkSpotAcceptRejectResult.reject());
    }

    public static ZLinkSpotActorJoinResult reject(ZLinkMessage reply) {
        return from(ZLinkSpotAcceptRejectResult.reject(reply));
    }

    public static ZLinkSpotActorJoinResult reject(Object reply) {
        return from(ZLinkSpotAcceptRejectResult.reject(reply));
    }

    private static ZLinkSpotActorJoinResult from(ZLinkSpotAcceptRejectResult result) {
        return new ZLinkSpotActorJoinResult(result.accepted(), result.reply());
    }
}
