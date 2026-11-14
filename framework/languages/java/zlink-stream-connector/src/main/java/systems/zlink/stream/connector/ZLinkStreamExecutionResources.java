package systems.zlink.stream.connector;

import java.net.ProxySelector;
import java.net.http.HttpClient;
import java.security.GeneralSecurityException;
import java.security.SecureRandom;
import java.security.cert.X509Certificate;
import java.util.concurrent.Callable;
import java.util.concurrent.Delayed;
import java.util.concurrent.ExecutionException;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;
import java.util.concurrent.RunnableScheduledFuture;
import java.util.concurrent.ScheduledExecutorService;
import java.util.concurrent.ScheduledThreadPoolExecutor;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.TimeoutException;

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
        protected <V> RunnableScheduledFuture<V> decorateTask(
                Runnable runnable, RunnableScheduledFuture<V> task) {
            return new TimerTask<>(task);
        }

        @Override
        protected <V> RunnableScheduledFuture<V> decorateTask(
                Callable<V> callable, RunnableScheduledFuture<V> task) {
            return new TimerTask<>(task);
        }

        private final class TimerTask<V> implements RunnableScheduledFuture<V> {
            private final RunnableScheduledFuture<V> task;

            private TimerTask(RunnableScheduledFuture<V> task) {
                this.task = task;
            }

            @Override
            public void run() {
                // The JDK future owns completion, failure, cancellation and periodic requeue.
                // It requeues this wrapper only after the body returns, so even a blocking
                // heartbeat never overlaps its next invocation. No second running flag is needed.
                WorkerHolder.INSTANCE.execute(task);
            }

            @Override
            public boolean cancel(boolean mayInterruptIfRunning) {
                boolean cancelled = task.cancel(mayInterruptIfRunning);
                // The delay queue contains the wrapper, not the JDK future it delegates to.
                if (cancelled) remove(this);
                return cancelled;
            }

            @Override
            public boolean isPeriodic() {
                return task.isPeriodic();
            }

            @Override
            public long getDelay(TimeUnit unit) {
                return task.getDelay(unit);
            }

            @Override
            public int compareTo(Delayed other) {
                return task.compareTo(
                        other instanceof TimerTask<?> timerTask ? timerTask.task : other);
            }

            @Override
            public boolean isCancelled() {
                return task.isCancelled();
            }

            @Override
            public boolean isDone() {
                return task.isDone();
            }

            @Override
            public V get() throws InterruptedException, ExecutionException {
                return task.get();
            }

            @Override
            public V get(long timeout, TimeUnit unit)
                    throws InterruptedException, ExecutionException, TimeoutException {
                return task.get(timeout, unit);
            }
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
