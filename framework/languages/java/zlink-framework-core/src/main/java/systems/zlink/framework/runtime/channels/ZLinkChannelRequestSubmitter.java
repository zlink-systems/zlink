package systems.zlink.framework.runtime.channels;

import java.time.Duration;
import java.util.List;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.ScheduledExecutorService;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.TimeoutException;
import systems.zlink.contracts.core.RoutingId;
import systems.zlink.contracts.errors.ZlinkSubmitException;
import systems.zlink.contracts.messaging.Message;
import systems.zlink.contracts.sockets.SendFlags;
import systems.zlink.contracts.sockets.SubmitResult;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendDealerSocket;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendReceived;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendRequestCallback;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendRequestResult;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendRouterSocket;

final class ZLinkChannelRequestSubmitter {
    private static final long RETRY_DELAY_MILLIS = 10;
    private static final int ERRNO_EAGAIN = 11;
    private static final int ERRNO_ENETUNREACH = 101;
    private static final int ERRNO_ENOTCONN = 107;
    private static final int ERRNO_ECONNREFUSED = 111;
    private static final int ERRNO_EHOSTUNREACH = 113;
    private static final int ERRNO_EWOULDBLOCK_WIN = 10035;
    private static final int ERRNO_ENETUNREACH_WIN = 10051;
    private static final int ERRNO_ENOTCONN_WIN = 10057;
    private static final int ERRNO_ECONNREFUSED_WIN = 10061;
    private static final int ERRNO_EHOSTUNREACH_WIN = 10065;

    private final ScheduledExecutorService scheduler;
    private final Duration defaultTimeout;

    ZLinkChannelRequestSubmitter(
        ScheduledExecutorService scheduler,
        Duration defaultTimeout) {
        this.scheduler = scheduler;
        this.defaultTimeout = defaultTimeout;
    }

    void submitRoute(
        ZLinkBackendRouterSocket router,
        RoutingId targetPeerRid,
        List<Message> requestParts,
        ZLinkBackendRequestCallback callback,
        Duration timeout,
        CompletableFuture<?> result) {
        List<byte[]> payloads = copyPayloads(requestParts);
        new RouteRequestAttempt(
            this,
            router,
            targetPeerRid,
            payloads,
            callback,
            effectiveTimeout(timeout),
            result).run();
    }

    void submitClient(
        ZLinkBackendDealerSocket client,
        List<Message> requestParts,
        Duration timeout,
        ZLinkBackendRequestCallback callback,
        CompletableFuture<?> result) {
        new ClientRequestAttempt(
            this,
            client,
            copyPayloads(requestParts),
            callback,
            effectiveTimeout(timeout),
            result).run();
    }

    void retry(Runnable attempt) {
        scheduler.schedule(attempt, RETRY_DELAY_MILLIS, TimeUnit.MILLISECONDS);
    }

    private Duration effectiveTimeout(Duration timeout) {
        return timeout == null || timeout.isZero() ? defaultTimeout : timeout;
    }

    private static List<byte[]> copyPayloads(List<Message> requestParts) {
        return requestParts.stream().map(Message::toByteArray).toList();
    }

    static List<Message> messages(List<byte[]> payloads) {
        return payloads.stream().map(Message::from).toList();
    }

    static boolean isRetriableSubmit(Throwable error) {
        Throwable current = error;
        while (current != null) {
            if (current instanceof ZlinkSubmitException submit) {
                // NOT_FOUND is a completed lookup result.  Do not reinterpret
                // an accompanying native errno as a transient transport state.
                if (submit.getResult() == SubmitResult.NOT_FOUND) {
                    return false;
                }
                int errno = submit.getNativeErrno();
                return submit.getResult() == SubmitResult.BACKPRESSURED
                    || submit.getResult() == SubmitResult.NOT_CONNECTED
                    || errno == ERRNO_EHOSTUNREACH
                    || errno == ERRNO_EHOSTUNREACH_WIN
                    || errno == ERRNO_ENETUNREACH
                    || errno == ERRNO_ENETUNREACH_WIN
                    || errno == ERRNO_ECONNREFUSED
                    || errno == ERRNO_ECONNREFUSED_WIN
                    || errno == ERRNO_ENOTCONN
                    || errno == ERRNO_ENOTCONN_WIN
                    || errno == ERRNO_EAGAIN
                    || errno == ERRNO_EWOULDBLOCK_WIN;
            }
            current = current.getCause();
        }
        return false;
    }
}

final class RouteRequestAttempt implements Runnable {
    private final ZLinkChannelRequestSubmitter submitter;
    private final ZLinkBackendRouterSocket router;
    private final RoutingId targetPeerRid;
    private final List<byte[]> payloads;
    private final ZLinkBackendRequestCallback callback;
    private final Duration timeout;
    private final long deadline;
    private final CompletableFuture<?> result;

    RouteRequestAttempt(
        ZLinkChannelRequestSubmitter submitter,
        ZLinkBackendRouterSocket router,
        RoutingId targetPeerRid,
        List<byte[]> payloads,
        ZLinkBackendRequestCallback callback,
        Duration timeout,
        CompletableFuture<?> result) {
        this.submitter = submitter;
        this.router = router;
        this.targetPeerRid = targetPeerRid;
        this.payloads = payloads;
        this.callback = callback;
        this.timeout = timeout;
        this.deadline = System.nanoTime() + timeout.toNanos();
        this.result = result;
    }

    @Override
    public void run() {
        if (result.isDone()) {
            return;
        }
        List<Message> attemptParts = ZLinkChannelRequestSubmitter.messages(payloads);
        try {
            ZLinkChannelRuntime.trace("route-request submit target=" + targetPeerRid
                + " parts=" + ZLinkChannelRuntime.describeTraceParts(attemptParts));
            boolean submitted = router.request(
                targetPeerRid,
                attemptParts,
                this::handleReply,
                SendFlags.DONT_WAIT,
                timeout);
            if (!submitted) {
                retryOrTimeout("routed SPOT route mesh request was not ready before timeout");
            }
        } catch (RuntimeException error) {
            if (ZLinkChannelRequestSubmitter.isRetriableSubmit(error)
                && System.nanoTime() < deadline) {
                ZLinkChannelRuntime.trace("route-request retry-exception target=" + targetPeerRid
                    + " error=" + ZLinkChannelRuntime.requestErrorSummary(error));
                submitter.retry(this);
            } else {
                result.completeExceptionally(error);
            }
        } finally {
            attemptParts.forEach(Message::close);
        }
    }

    private void handleReply(ZLinkBackendReceived reply) {
        ZLinkChannelRuntime.trace("route-request reply target=" + targetPeerRid
            + " result=" + reply.result()
            + " parts=" + ZLinkChannelRuntime.describeTraceParts(reply.parts()));
        // A callback result is the terminal completion of an accepted request.
        // The reply payload is application data, so it cannot be used to infer
        // whether the transport delivered the original request back to us.
        callback.handle(reply);
    }

    private void retryOrTimeout(String timeoutMessage) {
        if (System.nanoTime() >= deadline) {
            result.completeExceptionally(new TimeoutException(timeoutMessage));
        } else {
            ZLinkChannelRuntime.trace("route-request retry-submit target=" + targetPeerRid);
            submitter.retry(this);
        }
    }
}

final class ClientRequestAttempt implements Runnable {
    private final ZLinkChannelRequestSubmitter submitter;
    private final ZLinkBackendDealerSocket client;
    private final List<byte[]> payloads;
    private final ZLinkBackendRequestCallback callback;
    private final Duration timeout;
    private final long deadline;
    private final CompletableFuture<?> result;

    ClientRequestAttempt(
        ZLinkChannelRequestSubmitter submitter,
        ZLinkBackendDealerSocket client,
        List<byte[]> payloads,
        ZLinkBackendRequestCallback callback,
        Duration timeout,
        CompletableFuture<?> result) {
        this.submitter = submitter;
        this.client = client;
        this.payloads = payloads;
        this.callback = callback;
        this.timeout = timeout;
        this.deadline = System.nanoTime() + timeout.toNanos();
        this.result = result;
    }

    @Override
    public void run() {
        if (result.isDone()) {
            return;
        }
        List<Message> attemptParts = ZLinkChannelRequestSubmitter.messages(payloads);
        try {
            boolean submitted = client.request(
                attemptParts,
                callback,
                SendFlags.DONT_WAIT,
                timeout);
            if (submitted) {
                return;
            }
            retryOrTimeout();
        } catch (RuntimeException error) {
            if (ZLinkChannelRequestSubmitter.isRetriableSubmit(error)
                && System.nanoTime() < deadline) {
                submitter.retry(this);
            } else {
                result.completeExceptionally(error);
            }
        } finally {
            attemptParts.forEach(Message::close);
        }
    }

    private void retryOrTimeout() {
        if (System.nanoTime() >= deadline) {
            result.completeExceptionally(new TimeoutException(
                "channel request was not ready before timeout"));
        } else {
            submitter.retry(this);
        }
    }
}
