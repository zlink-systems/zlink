package systems.zlink.framework.spots;

import systems.zlink.framework.messaging.ZLinkMessage;

record ZLinkSpotAcceptRejectResult(
    boolean accepted,
    ZLinkMessage reply) {
    static ZLinkSpotAcceptRejectResult accept() {
        return accept((ZLinkMessage) null);
    }

    static ZLinkSpotAcceptRejectResult accept(ZLinkMessage reply) {
        return new ZLinkSpotAcceptRejectResult(true, reply);
    }

    static ZLinkSpotAcceptRejectResult accept(Object reply) {
        return accept(ZLinkMessage.of(reply));
    }

    static ZLinkSpotAcceptRejectResult reject() {
        return reject((ZLinkMessage) null);
    }

    static ZLinkSpotAcceptRejectResult reject(ZLinkMessage reply) {
        return new ZLinkSpotAcceptRejectResult(false, reply);
    }

    static ZLinkSpotAcceptRejectResult reject(Object reply) {
        return reject(ZLinkMessage.of(reply));
    }
}
