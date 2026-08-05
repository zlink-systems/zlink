/* SPDX-License-Identifier: Apache-2.0 */
package systems.zlink.httpclient.internal;

import java.io.ByteArrayInputStream;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.Path;
import java.security.KeyFactory;
import java.security.KeyStore;
import java.security.PrivateKey;
import java.security.cert.CertificateException;
import java.security.cert.CertificateFactory;
import java.security.cert.X509Certificate;
import java.security.spec.PKCS8EncodedKeySpec;
import java.util.Base64;
import javax.net.ssl.KeyManagerFactory;
import javax.net.ssl.SSLContext;
import javax.net.ssl.TrustManager;
import javax.net.ssl.TrustManagerFactory;
import javax.net.ssl.X509TrustManager;
import systems.zlink.framework.errors.ZLinkFrameworkErrorKind;
import systems.zlink.framework.errors.ZLinkFrameworkException;

/**
 * Builds an {@link SSLContext} from PEM files for the wrapper. A trust certificate is added as a
 * custom trust anchor; an mTLS client certificate and key are loaded into a key store. Mirrors the
 * dotnet/node TLS configuration so the {@code java.net.http} client validates and presents the same
 * certificates.
 */
public final class TlsSupport {

    private TlsSupport() {
    }

    public static SSLContext create(HttpClientOptions options) {
        try {
            TrustManager[] trustManagers = options.trustCertificateFile() != null
                ? trustManagers(options.trustCertificateFile())
                : null;
            KeyManagerFactory keyManagerFactory = options.clientCertificate() != null
                ? keyManagers(options.clientCertificate())
                : null;

            SSLContext context = SSLContext.getInstance("TLS");
            context.init(
                keyManagerFactory != null ? keyManagerFactory.getKeyManagers() : null,
                trustManagers,
                null);
            return context;
        } catch (Exception cause) {
            throw new ZLinkFrameworkException(ZLinkFrameworkErrorKind.PROTOCOL_ERROR, "HTTP client TLS configuration failed", cause);
        }
    }

    private static TrustManager[] trustManagers(String trustPath) throws Exception {
        X509Certificate certificate = readCertificate(Files.readAllBytes(Path.of(trustPath)));
        KeyStore customStore = KeyStore.getInstance(KeyStore.getDefaultType());
        customStore.load(null, null);
        customStore.setCertificateEntry("zlink-trust", certificate);
        // Add the custom certificate to the default JVM trust store rather than replacing it, so
        // public-CA HTTPS keeps working when a test/internal trust certificate is configured.
        X509TrustManager defaultTrust = firstX509(null);
        X509TrustManager customTrust = firstX509(customStore);
        return new TrustManager[] {new CompositeTrustManager(defaultTrust, customTrust)};
    }

    private static X509TrustManager firstX509(KeyStore store) throws Exception {
        TrustManagerFactory factory = TrustManagerFactory.getInstance(TrustManagerFactory.getDefaultAlgorithm());
        factory.init(store);
        for (TrustManager manager : factory.getTrustManagers()) {
            if (manager instanceof X509TrustManager x509) {
                return x509;
            }
        }
        throw new ZLinkFrameworkException(ZLinkFrameworkErrorKind.PROTOCOL_ERROR, "No X509 trust manager available");
    }

    /** Trusts a certificate if either the JVM default store or the custom certificate accepts it. */
    private static final class CompositeTrustManager implements X509TrustManager {
        private final X509TrustManager primary;
        private final X509TrustManager secondary;

        CompositeTrustManager(X509TrustManager primary, X509TrustManager secondary) {
            this.primary = primary;
            this.secondary = secondary;
        }

        @Override
        public void checkClientTrusted(X509Certificate[] chain, String authType) throws CertificateException {
            primary.checkClientTrusted(chain, authType);
        }

        @Override
        public void checkServerTrusted(X509Certificate[] chain, String authType) throws CertificateException {
            try {
                primary.checkServerTrusted(chain, authType);
            } catch (CertificateException ignored) {
                secondary.checkServerTrusted(chain, authType);
            }
        }

        @Override
        public X509Certificate[] getAcceptedIssuers() {
            X509Certificate[] a = primary.getAcceptedIssuers();
            X509Certificate[] b = secondary.getAcceptedIssuers();
            X509Certificate[] all = new X509Certificate[a.length + b.length];
            System.arraycopy(a, 0, all, 0, a.length);
            System.arraycopy(b, 0, all, a.length, b.length);
            return all;
        }
    }

    private static KeyManagerFactory keyManagers(HttpClientOptions.ClientCertificate certificate) throws Exception {
        X509Certificate cert = readCertificate(Files.readAllBytes(Path.of(certificate.certificatePath())));
        PrivateKey key = readPrivateKey(Files.readString(Path.of(certificate.keyPath())));
        KeyStore keyStore = KeyStore.getInstance(KeyStore.getDefaultType());
        keyStore.load(null, null);
        keyStore.setKeyEntry("zlink-client", key, new char[0], new X509Certificate[] {cert});
        KeyManagerFactory factory = KeyManagerFactory.getInstance(KeyManagerFactory.getDefaultAlgorithm());
        factory.init(keyStore, new char[0]);
        return factory;
    }

    private static X509Certificate readCertificate(byte[] pem) throws Exception {
        CertificateFactory factory = CertificateFactory.getInstance("X.509");
        return (X509Certificate) factory.generateCertificate(new ByteArrayInputStream(pem));
    }

    private static PrivateKey readPrivateKey(String pem) throws Exception {
        String base64 = pem
            .replaceAll("-----BEGIN (RSA )?PRIVATE KEY-----", "")
            .replaceAll("-----END (RSA )?PRIVATE KEY-----", "")
            .replaceAll("\\s", "");
        byte[] der = Base64.getDecoder().decode(base64.getBytes(StandardCharsets.US_ASCII));
        PKCS8EncodedKeySpec spec = new PKCS8EncodedKeySpec(der);
        return KeyFactory.getInstance("RSA").generatePrivate(spec);
    }
}
