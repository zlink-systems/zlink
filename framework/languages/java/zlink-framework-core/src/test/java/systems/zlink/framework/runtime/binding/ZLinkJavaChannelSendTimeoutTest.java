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
                var dealer = backend.createDealerSocket(context);
                var publisher = backend.createPublisherSocket(context)) {
            assertEquals(
                    Duration.ofSeconds(1),
                    ((ZLinkJavaSocketBacked) dealer).nativeSocket().options().sendTimeout());
            assertEquals(
                    Duration.ofSeconds(1),
                    ((ZLinkJavaSocketBacked) publisher).nativeSocket().options().sendTimeout());
        }
    }
}
