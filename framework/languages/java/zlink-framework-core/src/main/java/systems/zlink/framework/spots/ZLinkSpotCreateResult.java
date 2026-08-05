package systems.zlink.framework.spots;

import systems.zlink.framework.messaging.ZLinkMessage;

public record ZLinkSpotCreateResult(
    SpotRef spot,
    ZLinkSpotCreateState state,
    ZLinkMessage reply) {
}
