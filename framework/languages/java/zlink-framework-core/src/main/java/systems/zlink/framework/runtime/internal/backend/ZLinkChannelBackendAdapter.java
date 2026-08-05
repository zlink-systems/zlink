package systems.zlink.framework.runtime.internal.backend;

public interface ZLinkChannelBackendAdapter {
    ZLinkBackendContext createContext();

    ZLinkBackendDealerSocket createDealerSocket(ZLinkBackendContext context);

    ZLinkBackendRouterSocket createRouterSocket(ZLinkBackendContext context);

    ZLinkBackendPublisherSocket createPublisherSocket(ZLinkBackendContext context);

    ZLinkBackendSubscriberSocket createSubscriberSocket(ZLinkBackendContext context);
}
