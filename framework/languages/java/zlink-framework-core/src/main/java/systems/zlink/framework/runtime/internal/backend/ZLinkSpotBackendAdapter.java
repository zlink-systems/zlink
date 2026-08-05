package systems.zlink.framework.runtime.internal.backend;

import systems.zlink.framework.runtime.internal.backend.ZLinkInternalSpotNode;

public interface ZLinkSpotBackendAdapter {
    ZLinkInternalSpotNode createSpotNode(ZLinkBackendContext context, ZLinkBackendSpotNodeMode mode);
}
