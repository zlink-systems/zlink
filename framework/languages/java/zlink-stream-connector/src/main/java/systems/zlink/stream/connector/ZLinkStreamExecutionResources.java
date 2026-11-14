package systems.zlink.stream.connector;

import java.net.ProxySelector;
import java.net.http.HttpClient;
import java.security.GeneralSecurityException;
import java.security.SecureRandom;
import java.security.cert.X509Certificate;
import java.util.Objects;
import java.util.concurrent.Callable;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;
import java.util.concurrent.ScheduledExecutorService;
import java.util.concurrent.ScheduledFuture;
import java.util.concurrent.ScheduledThreadPoolExecutor;
import java.util.concurrent.TimeUnit;

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

    private static final class WorkerHolder {
        private static final ExecutorService INSTANCE =
                Executors.newCachedThreadPool(
                        runnable -> {
                            Thread thread = new Thread(runnable, "zlink-stream-connector-worker");
                            thread.setDaemon(true);
                            return thread;
                        });
    }

    private static final class TimerHolder {
        private static final ScheduledThreadPoolExecutor INSTANCE = new Timer();
    }

    private static final class Timer extends ScheduledThreadPoolExecutor {
        private Timer() {
            super(
                    1,
                    runnable -> {
                        Thread thread = new Thread(runnable, "zlink-stream-connector-timeouts");
                        thread.setDaemon(true);
                        return thread;
                    });
            // Cancelled deadlines must not retain closed connectors until their original expiry.
            setRemoveOnCancelPolicy(true);
        }

        @Override
        public ScheduledFuture<?> schedule(Runnable command, long delay, TimeUnit unit) {
            Objects.requireNonNull(command, "command");
            // Keep the JDK future in the queue so cancellation uses its heap index.
            return super.schedule(() -> WorkerHolder.INSTANCE.execute(command), delay, unit);
        }

        @Override
        public <V> ScheduledFuture<V> schedule(Callable<V> command, long delay, TimeUnit unit) {
            Objects.requireNonNull(command, "command");
            // This future tracks handoff, not the body's result or completion.
            return super.schedule(
                    () -> {
                        WorkerHolder.INSTANCE.submit(command);
                        return null;
                    },
                    delay,
                    unit);
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
        // One client per TLS policy shares the selector. Transport callbacks and timer bodies use
        // one cached pool, growing with concurrent work rather than connections. Blocking Immediate
        // handlers do not occupy the clock or the application's common pool.
        return HttpClient.newBuilder()
                .proxy(ProxySelector.of(null))
                .executor(WorkerHolder.INSTANCE);
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
