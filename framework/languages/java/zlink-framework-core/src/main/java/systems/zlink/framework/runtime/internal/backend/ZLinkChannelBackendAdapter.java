package systems.zlink.framework.runtime.internal.backend;

import java.time.Duration;

public interface ZLinkChannelBackendAdapter {
    ZLinkBackendContext createContext();

    ZLinkBackendDealerSocket createDealerSocket(ZLinkBackendContext context);

    default ZLinkBackendDealerSocket createDealerSocket(
            ZLinkBackendContext context, Duration sendTimeout) {
        return createDealerSocket(context);
    }

    ZLinkBackendRouterSocket createRouterSocket(ZLinkBackendContext context);

    ZLinkBackendPublisherSocket createPublisherSocket(ZLinkBackendContext context);

    default ZLinkBackendPublisherSocket createPublisherSocket(
            ZLinkBackendContext context, Duration sendTimeout) {
        return createPublisherSocket(context);
    }

    ZLinkBackendSubscriberSocket createSubscriberSocket(ZLinkBackendContext context);
}
