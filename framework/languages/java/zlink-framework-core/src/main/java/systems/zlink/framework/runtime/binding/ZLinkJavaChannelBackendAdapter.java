package systems.zlink.framework.runtime.binding;

import systems.zlink.contracts.core.Context;
import systems.zlink.contracts.core.Zlink;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendContext;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendDealerSocket;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendPublisherSocket;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendRouterSocket;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendSubscriberSocket;
import systems.zlink.framework.runtime.internal.backend.ZLinkChannelBackendAdapter;

import java.time.Duration;

final class ZLinkJavaChannelBackendAdapter implements ZLinkChannelBackendAdapter {
    @Override
    public ZLinkBackendContext createContext() {
        return new ZLinkJavaContext(Zlink.createContext());
    }

    @Override
    public ZLinkBackendDealerSocket createDealerSocket(ZLinkBackendContext context) {
        var socket =
                ZLinkJavaSocketOptions.configureFrameworkSocket(
                        nativeContext(context).createDealerSocket());
        return new ZLinkJavaDealerSocket(socket);
    }

    @Override
    public ZLinkBackendRouterSocket createRouterSocket(ZLinkBackendContext context) {
        return new ZLinkJavaRouterSocket(
                ZLinkJavaSocketOptions.configureFrameworkRouterSocket(
                        nativeContext(context).createRouterSocket()));
    }

    @Override
    public ZLinkBackendPublisherSocket createPublisherSocket(
            ZLinkBackendContext context, Duration sendTimeout) {
        var socket = nativeContext(context).createPubSocket();
        socket.options().linger(Duration.ZERO);
        socket.options().sendTimeout(sendTimeout);
        return new ZLinkJavaPublisherSocket(socket);
    }

    @Override
    public ZLinkBackendSubscriberSocket createSubscriberSocket(ZLinkBackendContext context) {
        return new ZLinkJavaSubscriberSocket(
                ZLinkJavaSocketOptions.configureFrameworkSocket(
                        nativeContext(context).createSubSocket()));
    }

    private static Context nativeContext(ZLinkBackendContext context) {
        return ((ZLinkJavaContext) context).nativeContext();
    }
}
