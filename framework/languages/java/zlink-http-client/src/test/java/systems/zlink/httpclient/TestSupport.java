/* SPDX-License-Identifier: Apache-2.0 */
package systems.zlink.httpclient;

import com.sun.net.httpserver.HttpExchange;
import com.sun.net.httpserver.HttpHandler;
import com.sun.net.httpserver.HttpServer;
import com.sun.net.httpserver.HttpsConfigurator;
import com.sun.net.httpserver.HttpsParameters;
import com.sun.net.httpserver.HttpsServer;
import java.io.ByteArrayInputStream;
import java.io.IOException;
import java.io.InputStream;
import java.net.InetSocketAddress;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.Path;
import java.security.KeyFactory;
import java.security.KeyStore;
import java.security.PrivateKey;
import java.security.cert.CertificateFactory;
import java.security.cert.X509Certificate;
import java.security.spec.PKCS8EncodedKeySpec;
import java.util.Base64;
import javax.net.ssl.KeyManagerFactory;
import javax.net.ssl.SSLContext;
import javax.net.ssl.SSLParameters;
import javax.net.ssl.TrustManager;
import javax.net.ssl.X509TrustManager;

/** Shared test servers and certificate helpers. */
final class TestSupport {

    private TestSupport() {
    }

    record Server(String baseUrl, AutoCloseable closeable) {
    }

    static String resourcePath(String resource) {
        try {
            return Path.of(TestSupport.class.getResource(resource).toURI()).toString();
        } catch (Exception cause) {
            throw new RuntimeException(cause);
        }
    }

    static Server httpServer(HttpHandler handler) {
        try {
            HttpServer server = HttpServer.create(new InetSocketAddress("127.0.0.1", 0), 0);
            // Multi-threaded executor so concurrent requests are not serialized by the test server.
            server.setExecutor(java.util.concurrent.Executors.newCachedThreadPool());
            server.createContext("/", handler);
            server.start();
            int port = server.getAddress().getPort();
            return new Server("http://127.0.0.1:" + port, () -> server.stop(0));
        } catch (IOException cause) {
            throw new RuntimeException(cause);
        }
    }

    static Server httpsServer(HttpHandler handler, boolean needClientAuth) {
        try {
            HttpsServer server = HttpsServer.create(new InetSocketAddress("127.0.0.1", 0), 0);
            SSLContext sslContext = serverSslContext();
            server.setHttpsConfigurator(new HttpsConfigurator(sslContext) {
                @Override
                public void configure(HttpsParameters params) {
                    SSLParameters sslParameters = sslContext.getDefaultSSLParameters();
                    sslParameters.setNeedClientAuth(needClientAuth);
                    params.setSSLParameters(sslParameters);
                }
            });
            server.createContext("/", handler);
            server.start();
            int port = server.getAddress().getPort();
            return new Server("https://127.0.0.1:" + port, () -> server.stop(0));
        } catch (Exception cause) {
            throw new RuntimeException(cause);
        }
    }

    static void respond(HttpExchange exchange, int status, String body) throws IOException {
        byte[] bytes = body.getBytes(StandardCharsets.UTF_8);
        if ("HEAD".equals(exchange.getRequestMethod())) {
            exchange.sendResponseHeaders(status, -1);
            exchange.close();
            return;
        }
        exchange.sendResponseHeaders(status, bytes.length == 0 ? -1 : bytes.length);
        try (var out = exchange.getResponseBody()) {
            out.write(bytes);
        }
    }

    static void respondBytes(HttpExchange exchange, int status, byte[] bytes) throws IOException {
        exchange.sendResponseHeaders(status, bytes.length);
        try (var out = exchange.getResponseBody()) {
            out.write(bytes);
        }
    }

    static String readBody(HttpExchange exchange) throws IOException {
        return new String(exchange.getRequestBody().readAllBytes(), StandardCharsets.UTF_8);
    }

    private static SSLContext serverSslContext() throws Exception {
        X509Certificate cert = readCertificate(Files.readAllBytes(Path.of(resourcePath("/tls/server-cert.pem"))));
        PrivateKey key = readPrivateKey(Files.readString(Path.of(resourcePath("/tls/server-key.pem"))));
        KeyStore keyStore = KeyStore.getInstance(KeyStore.getDefaultType());
        keyStore.load(null, null);
        keyStore.setKeyEntry("server", key, new char[0], new X509Certificate[] {cert});
        KeyManagerFactory keyManagerFactory = KeyManagerFactory.getInstance(KeyManagerFactory.getDefaultAlgorithm());
        keyManagerFactory.init(keyStore, new char[0]);

        TrustManager trustAll = new X509TrustManager() {
            @Override
            public void checkClientTrusted(X509Certificate[] chain, String authType) {
            }

            @Override
            public void checkServerTrusted(X509Certificate[] chain, String authType) {
            }

            @Override
            public X509Certificate[] getAcceptedIssuers() {
                return new X509Certificate[0];
            }
        };

        SSLContext context = SSLContext.getInstance("TLS");
        context.init(keyManagerFactory.getKeyManagers(), new TrustManager[] {trustAll}, null);
        return context;
    }

    private static X509Certificate readCertificate(byte[] pem) throws Exception {
        return (X509Certificate) CertificateFactory.getInstance("X.509")
            .generateCertificate(new ByteArrayInputStream(pem));
    }

    private static PrivateKey readPrivateKey(String pem) throws Exception {
        String base64 = pem
            .replaceAll("-----BEGIN (RSA )?PRIVATE KEY-----", "")
            .replaceAll("-----END (RSA )?PRIVATE KEY-----", "")
            .replaceAll("\\s", "");
        byte[] der = Base64.getDecoder().decode(base64);
        return KeyFactory.getInstance("RSA").generatePrivate(new PKCS8EncodedKeySpec(der));
    }

    static byte[] readAll(InputStream stream) throws IOException {
        return stream.readAllBytes();
    }
}
