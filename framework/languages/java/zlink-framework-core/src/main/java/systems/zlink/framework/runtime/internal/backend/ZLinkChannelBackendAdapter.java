package systems.zlink.framework.runtime.internal.backend;

import java.time.Duration;

public interface ZLinkChannelBackendAdapter {
    ZLinkBackendContext createContext();

    ZLinkBackendDealerSocket createDealerSocket(ZLinkBackendContext context, Duration sendTimeout);

    ZLinkBackendRouterSocket createRouterSocket(ZLinkBackendContext context);

    ZLinkBackendPublisherSocket createPublisherSocket(
            ZLinkBackendContext context, Duration sendTimeout);

    ZLinkBackendSubscriberSocket createSubscriberSocket(ZLinkBackendContext context);
}
