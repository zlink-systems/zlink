package systems.zlink.framework.spots;

import systems.zlink.framework.messaging.ZLinkMessage;

public record ZLinkSpotCreateResponse(
    boolean accepted,
    ZLinkMessage reply) {
    public static ZLinkSpotCreateResponse accept() {
        return from(ZLinkSpotAcceptRejectResult.accept());
    }

    public static ZLinkSpotCreateResponse accept(ZLinkMessage reply) {
        return from(ZLinkSpotAcceptRejectResult.accept(reply));
    }

    public static ZLinkSpotCreateResponse accept(Object reply) {
        return from(ZLinkSpotAcceptRejectResult.accept(reply));
    }

    public static ZLinkSpotCreateResponse reject() {
        return from(ZLinkSpotAcceptRejectResult.reject());
    }

    public static ZLinkSpotCreateResponse reject(ZLinkMessage reply) {
        return from(ZLinkSpotAcceptRejectResult.reject(reply));
    }

    public static ZLinkSpotCreateResponse reject(Object reply) {
        return from(ZLinkSpotAcceptRejectResult.reject(reply));
    }

    private static ZLinkSpotCreateResponse from(ZLinkSpotAcceptRejectResult result) {
        return new ZLinkSpotCreateResponse(result.accepted(), result.reply());
    }
}
