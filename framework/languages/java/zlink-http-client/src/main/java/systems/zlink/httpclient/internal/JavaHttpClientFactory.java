/* SPDX-License-Identifier: Apache-2.0 */
package systems.zlink.httpclient.internal;

import java.net.InetSocketAddress;
import java.net.Authenticator;
import java.net.PasswordAuthentication;
import java.net.ProxySelector;
import java.net.URI;
import java.net.http.HttpClient;
import java.util.concurrent.Executor;

/**
 * Builds the underlying {@link HttpClient} for the wrapper: native auto-redirect is disabled (the
 * wrapper drives redirects), HTTP/1.1 is pinned, and proxy and TLS (trust certificate + mTLS client
 * certificate) are configured. Connection pooling stays native. Separated from
 * {@link HttpClientRuntime} so client construction is independent of request orchestration.
 */
final class JavaHttpClientFactory {

    private JavaHttpClientFactory() {
    }

    static HttpClient create(HttpClientOptions options, Executor executor) {
        HttpClient.Builder builder = HttpClient.newBuilder()
            .version(HttpClient.Version.HTTP_1_1)
            .followRedirects(HttpClient.Redirect.NEVER)
            .connectTimeout(options.timeout())
            .executor(executor);
        if (options.proxy() != null) {
            URI proxyUri = URI.create(options.proxy());
            builder.proxy(ProxySelector.of(new InetSocketAddress(proxyUri.getHost(), proxyUri.getPort())));
            if (options.proxyAuthorization() != null) {
                PasswordAuthentication credentials = decodeBasic(options.proxyAuthorization());
                builder.authenticator(new Authenticator() {
                    @Override
                    protected PasswordAuthentication getPasswordAuthentication() {
                        return getRequestorType() == RequestorType.PROXY
                            ? credentials
                            : null;
                    }
                });
            }
        }
        if (options.trustCertificateFile() != null || options.clientCertificate() != null) {
            builder.sslContext(TlsSupport.create(options));
        }
        return builder.build();
    }

    private static PasswordAuthentication decodeBasic(String authorization) {
        String prefix = "Basic ";
        if (!authorization.startsWith(prefix)) {
            throw new IllegalArgumentException("proxy authorization must use Basic authentication");
        }
        String decoded = new String(
            java.util.Base64.getDecoder().decode(authorization.substring(prefix.length())),
            java.nio.charset.StandardCharsets.UTF_8);
        int separator = decoded.indexOf(':');
        String user = separator < 0 ? decoded : decoded.substring(0, separator);
        String password = separator < 0 ? "" : decoded.substring(separator + 1);
        return new PasswordAuthentication(user, password.toCharArray());
    }
}
