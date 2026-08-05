package systems.zlink.framework.spots;

import systems.zlink.framework.messaging.ZLinkMessage;

/**
 * Entry Spot admission result for the first creation of an Actor.
 */
public record ZLinkActorCreateResponse(boolean accepted, ZLinkMessage reply) {
    public static ZLinkActorCreateResponse accept() {
        return new ZLinkActorCreateResponse(true, null);
    }

    public static ZLinkActorCreateResponse accept(ZLinkMessage reply) {
        return new ZLinkActorCreateResponse(true, reply);
    }

    public static ZLinkActorCreateResponse accept(Object reply) {
        return accept(ZLinkMessage.of(reply));
    }

    public static ZLinkActorCreateResponse reject() {
        return new ZLinkActorCreateResponse(false, null);
    }

    public static ZLinkActorCreateResponse reject(ZLinkMessage reply) {
        return new ZLinkActorCreateResponse(false, reply);
    }

    public static ZLinkActorCreateResponse reject(Object reply) {
        return reject(ZLinkMessage.of(reply));
    }
}
