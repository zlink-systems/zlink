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
                var dealer = backend.createDealerSocket(context, Duration.ofSeconds(1));
                var publisher = backend.createPublisherSocket(context, Duration.ofSeconds(1))) {
            assertEquals(
                    Duration.ofSeconds(1),
                    ((ZLinkJavaSocketBacked) dealer).nativeSocket().options().sendTimeout());
            assertEquals(
                    Duration.ofSeconds(1),
                    ((ZLinkJavaSocketBacked) publisher).nativeSocket().options().sendTimeout());
        }
    }

    @Test
    void configuredClientDealerAndFanoutPublisherUseTheRequestedSendTimeout() {
        Duration clientTimeout = Duration.ofMillis(375);
        Duration publisherTimeout = Duration.ofMillis(625);
        ZLinkJavaChannelBackendAdapter backend = new ZLinkJavaChannelBackendAdapter();
        try (ZLinkBackendContext context = backend.createContext();
                var dealer = backend.createDealerSocket(context, clientTimeout);
                var publisher = backend.createPublisherSocket(context, publisherTimeout)) {
            assertEquals(
                    clientTimeout,
                    ((ZLinkJavaSocketBacked) dealer).nativeSocket().options().sendTimeout());
            assertEquals(
                    publisherTimeout,
                    ((ZLinkJavaSocketBacked) publisher).nativeSocket().options().sendTimeout());
        }
    }
}
