package systems.zlink.framework.locations.etcd;

import static org.junit.jupiter.api.Assertions.*;

import io.grpc.netty.shaded.io.grpc.netty.GrpcSslContexts;
import io.grpc.netty.shaded.io.netty.handler.ssl.ClientAuth;

import org.junit.jupiter.api.Test;

import systems.zlink.framework.locationprovider.*;

import java.nio.charset.StandardCharsets;
import java.time.Duration;
import java.util.List;

class ZLinkEtcdTlsTest {
    static byte[] resource(String name) throws Exception {
        try (var input = ZLinkEtcdTlsTest.class.getResourceAsStream("/tls/" + name)) {
            assertNotNull(input);
            return input.readAllBytes();
        }
    }

    static String pem(String name) throws Exception {
        return new String(resource(name), StandardCharsets.US_ASCII);
    }

    @Test
    void customCaAndClientIdentityEstablishMutualTlsAndUntrustedCaFails() throws Exception {
        var context =
                GrpcSslContexts.forServer(
                                new java.io.ByteArrayInputStream(resource("server.pem")),
                                new java.io.ByteArrayInputStream(resource("server.key")))
                        .trustManager(new java.io.ByteArrayInputStream(resource("ca.pem")))
                        .clientAuth(ClientAuth.REQUIRE)
                        .build();
        try (var backend = new EtcdTestServer(context)) {
            String endpoint = backend.endpoint().replace("http:", "https:");
            var identity = new ZLinkEtcdClientIdentity(pem("client.pem"), pem("client.key"));
            var tls =
                    new ZLinkEtcdTlsOptions()
                            .setTrustedCaPem(pem("ca.pem"))
                            .setClientIdentity(identity);
            try (var store =
                    new ZLinkEtcdLocationStore(
                            backend.options().setEndpoints(List.of(endpoint)).setTls(tls))) {
                assertInstanceOf(
                        ZLinkStoreReadMissing.class,
                        ZLinkEtcdProviderTest.await(
                                store.read(ZLinkEtcdProviderTest.KEY, () -> false)));
            }
            int calls = backend.txnCalls.get();
            try (var store =
                    new ZLinkEtcdLocationStore(
                            backend.options()
                                    .setEndpoints(List.of(endpoint))
                                    .setOperationTimeout(Duration.ofMillis(500))
                                    .setTls(
                                            new ZLinkEtcdTlsOptions()
                                                    .setClientIdentity(identity)))) {
                assertThrows(
                        Exception.class,
                        () ->
                                ZLinkEtcdProviderTest.await(
                                        store.read(ZLinkEtcdProviderTest.KEY, () -> false)));
            }
            assertEquals(
                    calls,
                    backend.txnCalls.get(),
                    "Untrusted CA must fail before a Store RPC reaches the server");
            try (var store =
                    new ZLinkEtcdLocationStore(
                            backend.options()
                                    .setEndpoints(
                                            List.of(endpoint.replace("localhost", "127.0.0.1")))
                                    .setOperationTimeout(Duration.ofMillis(500))
                                    .setTls(tls))) {
                assertThrows(
                        Exception.class,
                        () ->
                                ZLinkEtcdProviderTest.await(
                                        store.read(ZLinkEtcdProviderTest.KEY, () -> false)));
            }
            assertEquals(
                    calls,
                    backend.txnCalls.get(),
                    "Host name mismatch must fail before a Store RPC reaches the server");
        }
    }
}
