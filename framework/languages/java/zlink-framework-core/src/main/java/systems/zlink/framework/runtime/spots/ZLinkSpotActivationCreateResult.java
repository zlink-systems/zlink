package systems.zlink.framework.runtime.spots;

import systems.zlink.framework.spots.ZLinkSpotCreateResponse;

record SpotActivationCreateResult(
    SpotActivation activation,
    ZLinkSpotCreateResponse response) {
}
