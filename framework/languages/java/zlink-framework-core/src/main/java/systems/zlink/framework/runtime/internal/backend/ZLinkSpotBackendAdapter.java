package systems.zlink.framework.runtime.internal.backend;

public interface ZLinkSpotBackendAdapter {
    ZLinkInternalSpotNode createSpotNode(
            ZLinkBackendContext context, ZLinkBackendSpotNodeMode mode);
}
