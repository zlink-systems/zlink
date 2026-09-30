package systems.zlink.framework.testkit;

import static org.junit.jupiter.api.Assertions.assertEquals;

import org.junit.jupiter.api.Test;

import systems.zlink.framework.runtime.internal.backend.ZLinkBackendAdapterOptions;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendContext;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendSpotNodeMode;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendStreamSocket;
import systems.zlink.framework.runtime.internal.backend.ZLinkChannelBackendAdapter;
import systems.zlink.framework.runtime.internal.backend.ZLinkInternalSpotNode;

import java.time.Duration;
import java.util.List;

final class FakeZLinkBackendAdapterTest {
    @Test
    void configuredSendTimeoutReachesFakeSocketCreation() {
        var factory = new FakeZLinkBackendAdapterFactory();
        var channel =
                factory.createChannelAdapter(new ZLinkBackendAdapterOptions(Duration.ofSeconds(1)));
        var context = channel.createContext();
        channel.createDealerSocket(context, Duration.ofMillis(375));
        channel.createPublisherSocket(context, Duration.ofMillis(625));
        org.junit.jupiter.api.Assertions.assertTrue(
                factory.calls().contains("create.dealer.sendTimeout.PT0.375S"));
        org.junit.jupiter.api.Assertions.assertTrue(
                factory.calls().contains("create.publisher.sendTimeout.PT0.625S"));
    }

    @Test
    void fakeBackendMirrorsSubsystemAdapterBoundaryWithoutBindingConcreteTypes() {
        FakeZLinkBackendAdapterFactory factory = new FakeZLinkBackendAdapterFactory();
        ZLinkBackendAdapterOptions options = new ZLinkBackendAdapterOptions(Duration.ofSeconds(1));
        ZLinkChannelBackendAdapter channel = factory.createChannelAdapter(options);
        ZLinkBackendContext context = channel.createContext();
        channel.createDealerSocket(context, Duration.ofSeconds(1));

        ZLinkInternalSpotNode spotNode =
                factory.createSpotAdapter(options)
                        .createSpotNode(context, ZLinkBackendSpotNodeMode.ALL);
        ZLinkBackendStreamSocket stream =
                factory.createStreamAdapter(options).createStreamSocket(context, null);

        factory.createMonitoringAdapter(options).openSocketMonitor(stream);

        assertEquals(
                List.of(
                        "factory.channel",
                        "create.context",
                        "create.dealer.sendTimeout.PT1S",
                        "create.dealer",
                        "factory.spot",
                        "create.spotNode",
                        "factory.stream",
                        "create.stream",
                        "factory.monitoring",
                        "monitoring.open.stream",
                        "create.socketMonitor"),
                factory.calls());
    }
}
