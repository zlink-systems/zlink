package systems.zlink.framework.runtime.binding;

import static org.junit.jupiter.api.Assertions.assertEquals;

import org.junit.jupiter.api.Test;

import systems.zlink.framework.runtime.internal.backend.ZLinkBackendContext;

import java.time.Duration;

final class ZLinkJavaChannelSendTimeoutTest {
    @Test
    void clientDealerAndFanoutPublisherReceiveSendTimeoutAtCreation() {
        ZLinkJavaChannelBackendAdapter backend = new ZLinkJavaChannelBackendAdapter();
        try (ZLinkBackendContext context = backend.createContext();
                var defaultDealer =
                        ((ZLinkJavaContext) context).nativeContext().createDealerSocket();
                var dealer = backend.createDealerSocket(context);
                var publisher = backend.createPublisherSocket(context, Duration.ofSeconds(1))) {
            assertEquals(
                    defaultDealer.options().sendTimeout(),
                    ((ZLinkJavaSocketBacked) dealer).nativeSocket().options().sendTimeout());
            assertEquals(
                    Duration.ofSeconds(1),
                    ((ZLinkJavaSocketBacked) publisher).nativeSocket().options().sendTimeout());
        }
    }

    @Test
    void configuredClientDealerAndFanoutPublisherUseTheRequestedSendTimeout() {
        Duration publisherTimeout = Duration.ofMillis(625);
        ZLinkJavaChannelBackendAdapter backend = new ZLinkJavaChannelBackendAdapter();
        try (ZLinkBackendContext context = backend.createContext();
                var defaultDealer =
                        ((ZLinkJavaContext) context).nativeContext().createDealerSocket();
                var dealer = backend.createDealerSocket(context);
                var publisher = backend.createPublisherSocket(context, publisherTimeout)) {
            assertEquals(
                    defaultDealer.options().sendTimeout(),
                    ((ZLinkJavaSocketBacked) dealer).nativeSocket().options().sendTimeout());
            assertEquals(
                    publisherTimeout,
                    ((ZLinkJavaSocketBacked) publisher).nativeSocket().options().sendTimeout());
        }
    }
}
