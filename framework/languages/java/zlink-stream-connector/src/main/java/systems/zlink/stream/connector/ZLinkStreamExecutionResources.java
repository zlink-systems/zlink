package systems.zlink.stream.connector;

import java.net.ProxySelector;
import java.net.http.HttpClient;
import java.security.GeneralSecurityException;
import java.security.SecureRandom;
import java.security.cert.X509Certificate;
import java.util.concurrent.ScheduledExecutorService;
import java.util.concurrent.ScheduledThreadPoolExecutor;

import javax.net.ssl.SSLContext;
import javax.net.ssl.TrustManager;
import javax.net.ssl.X509TrustManager;

/** Process resources: connector close releases its transport and tasks, never these resources. */
final class ZLinkStreamExecutionResources {
    private ZLinkStreamExecutionResources() {}

    static ScheduledExecutorService timeouts() {
        return TimerHolder.INSTANCE;
    }

    static HttpClient webSocketClient(boolean skipServerCertificateValidation) {
        return skipServerCertificateValidation
                ? InsecureClientHolder.INSTANCE
                : VerifiedClientHolder.INSTANCE;
    }

    private static final class TimerHolder {
        private static final ScheduledThreadPoolExecutor INSTANCE = create();

        private static ScheduledThreadPoolExecutor create() {
            ScheduledThreadPoolExecutor scheduler =
                    new ScheduledThreadPoolExecutor(
                            1,
                            runnable -> {
                                Thread thread =
                                        new Thread(runnable, "zlink-stream-connector-timeouts");
                                thread.setDaemon(true);
                                return thread;
                            });
            // Cancelled deadlines must not retain closed connectors until their original expiry.
            scheduler.setRemoveOnCancelPolicy(true);
            return scheduler;
        }
    }

    // Each currently supported proxy policy is direct. TLS policies use distinct, lazy clients;
    // endpoints, connection attempts and reconnects do not create clients or executor pools.
    private static final class VerifiedClientHolder {
        private static final HttpClient INSTANCE = clientBuilder().build();
    }

    private static final class InsecureClientHolder {
        private static final HttpClient INSTANCE =
                clientBuilder().sslContext(insecureSslContext()).build();
    }

    private static HttpClient.Builder clientBuilder() {
        // One client per TLS policy keeps the JDK selector and its default executor shared by all
        // connections. The default executor grows with concurrent callbacks, not with
        // connections, and is not the application's common pool, so a blocking Immediate
        // handler cannot starve unrelated work.
        return HttpClient.newBuilder().proxy(ProxySelector.of(null));
    }

    private static SSLContext insecureSslContext() {
        try {
            TrustManager[] trustAllManagers =
                    new TrustManager[] {
                        new X509TrustManager() {
                            @Override
                            public void checkClientTrusted(
                                    X509Certificate[] chain, String authType) {}

                            @Override
                            public void checkServerTrusted(
                                    X509Certificate[] chain, String authType) {}

                            @Override
                            public X509Certificate[] getAcceptedIssuers() {
                                return new X509Certificate[0];
                            }
                        }
                    };
            SSLContext context = SSLContext.getInstance("TLS");
            context.init(null, trustAllManagers, new SecureRandom());
            return context;
        } catch (GeneralSecurityException ex) {
            throw ZLinkStreamException.configurationError(
                    "failed to create insecure TLS context", ex);
        }
    }
}
