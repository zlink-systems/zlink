package systems.zlink.stream.connector;

import static org.junit.jupiter.api.Assertions.assertEquals;

import org.junit.jupiter.api.Test;
import org.junit.jupiter.api.parallel.Isolated;

import java.io.IOException;
import java.net.Proxy;
import java.net.ProxySelector;
import java.net.SocketAddress;
import java.net.URI;
import java.util.List;
import java.util.concurrent.atomic.AtomicInteger;

@Isolated("Changes the process default ProxySelector")
final class ZLinkStreamDirectConnectionTest {
    @Test
    void webSocketConnectDoesNotConsultDefaultProxySelector() throws Exception {
        AtomicInteger selections = new AtomicInteger();
        ProxySelector original = ProxySelector.getDefault();
        ProxySelector recordingSelector =
                new ProxySelector() {
                    @Override
                    public List<Proxy> select(URI uri) {
                        selections.incrementAndGet();
                        return List.of(Proxy.NO_PROXY);
                    }

                    @Override
                    public void connectFailed(URI uri, SocketAddress address, IOException failure) {
                        throw new AssertionError("Unexpected proxy connection failure", failure);
                    }
                };
        try (WebSocketStreamConnectorTestServer server = new WebSocketStreamConnectorTestServer()) {
            ProxySelector.setDefault(recordingSelector);
            ZLinkStreamConnector connector =
                    ZLinkStreamConnectorFactory.create(
                            server.options(ZLinkStreamDispatchMode.IMMEDIATE));
            try {
                ConnectorTestAwait.await(connector.connect());
                assertEquals(0, selections.get());
            } finally {
                ConnectorTestAwait.await(connector.close());
            }
        } finally {
            ProxySelector.setDefault(original);
        }
    }
}
