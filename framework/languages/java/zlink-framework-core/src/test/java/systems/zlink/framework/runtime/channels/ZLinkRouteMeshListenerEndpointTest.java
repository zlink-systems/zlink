package systems.zlink.framework.runtime.channels;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertTrue;

import org.junit.jupiter.params.ParameterizedTest;
import org.junit.jupiter.params.provider.ValueSource;

import systems.zlink.contracts.core.RoutingId;
import systems.zlink.framework.runtime.binding.ZLinkJavaBackendAdapterFactory;
import systems.zlink.framework.runtime.configuration.DefaultZLinkFrameworkOptions;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendAdapterOptions;
import systems.zlink.framework.runtime.internal.configuration.ZLinkLegacyTopology;
import systems.zlink.framework.runtime.messaging.ZLinkJsonMessageSerializer;

import java.net.URI;
import java.time.Duration;

final class ZLinkRouteMeshListenerEndpointTest {
    @ParameterizedTest
    @ValueSource(strings = {"*", "0"})
    void eachBindPublishesItsOwnActualPort(String port) {
        var options = new DefaultZLinkFrameworkOptions();
        ZLinkLegacyTopology.addRouteMeshChannel(options, "listener-route")
                .setRoutingId(RoutingId.from("listener-route"))
                .enableServer("tcp://127.0.0.1:" + port)
                .enableServer("tcp://127.0.0.1:" + port);
        var backend =
                new ZLinkJavaBackendAdapterFactory()
                        .createChannelAdapter(
                                new ZLinkBackendAdapterOptions(Duration.ofSeconds(1)));
        try (var runtime =
                new ZLinkChannelRuntime(
                        backend, options.registration(), new ZLinkJsonMessageSerializer())) {
            var surfaces = runtime.autoConnectSurfaces();
            assertEquals(2, surfaces.size());
            var endpoints =
                    surfaces.stream()
                            .map(ZLinkChannelRuntime.AutoConnectSurface::endpoint)
                            .toList();
            assertEquals(2, endpoints.stream().distinct().count(), endpoints.toString());
            for (String endpoint : endpoints) {
                int boundPort = URI.create(endpoint).getPort();
                assertTrue(boundPort > 0 && boundPort <= 65_535, endpoint);
            }
        }
    }
}
