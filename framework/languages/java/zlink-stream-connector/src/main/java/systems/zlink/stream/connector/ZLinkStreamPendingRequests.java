package systems.zlink.stream.connector;

import java.time.Duration;
import java.util.ArrayList;
import java.util.Set;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.ConcurrentHashMap;
import java.util.concurrent.ScheduledExecutorService;
import java.util.concurrent.ScheduledFuture;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.TimeoutException;
import java.util.function.BiFunction;
import java.util.function.BooleanSupplier;
import java.util.function.Consumer;
import java.util.function.Function;
import java.util.function.IntSupplier;
import java.util.function.LongSupplier;
import java.util.function.Supplier;

final class ZLinkStreamPendingRequests {
    // The logical request stays active between attempts, when it has no sequence to match.
    private final Set<PendingRequest> active = ConcurrentHashMap.newKeySet();
    private final ConcurrentHashMap<Long, PendingRequest> attempts = new ConcurrentHashMap<>();
    private final ScheduledExecutorService scheduler;
    private final LongSupplier clock;
    private final IntSupplier jitterMillis;

    ZLinkStreamPendingRequests(
            ScheduledExecutorService scheduler, LongSupplier clock, IntSupplier jitterMillis) {
        this.scheduler = scheduler;
        this.clock = clock;
        this.jitterMillis = jitterMillis;
    }

    long nanoTime() {
        return clock.getAsLong();
    }

    PendingRequest start(
            ZLinkStreamTransportConnection owner,
            String packetName,
            Duration timeout,
            Duration maxAdmissionWait,
            BiFunction<ZLinkStreamEncodedPayload, BooleanSupplier, Boolean> onReply,
            BiFunction<Throwable, BooleanSupplier, Boolean> onFailure,
            Consumer<Duration> onAdmissionWaiting,
            Consumer<PendingRequest> retry) {
        long timeoutNanos = boundedNanos(timeout);
        PendingRequest request =
                new PendingRequest(
                        owner,
                        packetName,
                        timeout,
                        nanoTime() + timeoutNanos,
                        maxAdmissionWait == null ? timeoutNanos : boundedNanos(maxAdmissionWait),
                        onReply,
                        onFailure,
                        onAdmissionWaiting,
                        retry);
        request.start();
        return request;
    }

    boolean admissionRejected(long sequence, String hint, long receivedAt) {
        PendingRequest request = attempts.get(sequence);
        return request != null && request.admissionRejected(sequence, hint, receivedAt);
    }

    void complete(long sequence, Supplier<ZLinkStreamEncodedPayload> decode) {
        PendingRequest request = attempts.get(sequence);
        if (request == null) {
            return;
        }
        ZLinkStreamEncodedPayload payload = decode.get();
        boolean completed = false;
        try {
            if (!request.terminateAttempt(sequence)) {
                return;
            }
            if (request.deadline - nanoTime() <= 0) {
                request.publishFailure(request.timeoutFailure());
            } else {
                var reply =
                        new ZLinkStreamEncodedPayload(
                                request.packetName,
                                payload.payload(),
                                payload.metadata(),
                                payload.codec());
                completed = request.onReply.apply(reply, () -> request.future.complete(reply));
            }
        } catch (RuntimeException failure) {
            request.publishFailure(failure);
        } finally {
            if (!completed) {
                payload.payload().close();
            }
        }
    }

    boolean fail(long sequence, Throwable failure) {
        PendingRequest request = attempts.get(sequence);
        if (request == null || !request.terminateAttempt(sequence)) {
            return false;
        }
        request.publishFailure(failure);
        return true;
    }

    /**
     * Fails requests owned by the ended transport, including those waiting between attempts. A null
     * owner means the connector is closing. The ending cause is preserved.
     */
    void failAll(Throwable ex, ZLinkStreamTransportConnection owner) {
        ZLinkStreamException failure =
                ex instanceof ZLinkStreamException coded
                                && coded.errorCode() == ZLinkStreamErrorCode.DISCONNECTED
                        ? coded
                        : ZLinkStreamException.of(
                                ZLinkStreamErrorCode.DISCONNECTED,
                                "connection ended: " + coded(ex).getMessage(),
                                ex);
        var ended = new ArrayList<PendingRequest>();
        for (PendingRequest request : active) {
            if ((owner == null || request.owner == owner) && request.terminate()) {
                ended.add(request);
            }
        }
        for (PendingRequest request : ended) {
            request.publishFailure(failure);
        }
    }

    private static int admissionDelayMillis(String hint) {
        try {
            int value = Integer.parseInt(hint);
            return value >= 0 && Integer.toString(value).equals(hint) ? value : -1;
        } catch (NumberFormatException invalid) {
            return -1;
        }
    }

    private static long boundedNanos(Duration duration) {
        try {
            return duration.toNanos();
        } catch (ArithmeticException overflow) {
            return Long.MAX_VALUE;
        }
    }

    static ZLinkStreamException coded(Throwable ex) {
        if (ex instanceof ZLinkStreamException coded) {
            return coded;
        }
        String message =
                ex.getMessage() == null || ex.getMessage().isBlank()
                        ? ex.getClass().getSimpleName()
                        : ex.getMessage();
        return ZLinkStreamException.of(ZLinkStreamErrorCode.DISCONNECTED, message, ex);
    }

    final class PendingRequest {
        final ZLinkStreamTransportConnection owner;
        final String packetName;
        final Duration timeout;
        final long admissionBudget;
        final BiFunction<ZLinkStreamEncodedPayload, BooleanSupplier, Boolean> onReply;
        final BiFunction<Throwable, BooleanSupplier, Boolean> onFailure;
        final Consumer<Duration> onAdmissionWaiting;
        final Consumer<PendingRequest> retry;
        final RequestFuture future;

        // The request monitor protects the current attempt, deadline and terminal claim.
        // A null timeout task denotes a terminated request after start.
        private long deadline;
        private long sequence;
        private CompletableFuture<Void> write;
        private ScheduledFuture<?> timeoutTask;
        private ScheduledFuture<?> retryTask;

        private PendingRequest(
                ZLinkStreamTransportConnection owner,
                String packetName,
                Duration timeout,
                long deadline,
                long admissionBudget,
                BiFunction<ZLinkStreamEncodedPayload, BooleanSupplier, Boolean> onReply,
                BiFunction<Throwable, BooleanSupplier, Boolean> onFailure,
                Consumer<Duration> onAdmissionWaiting,
                Consumer<PendingRequest> retry) {
            this.owner = owner;
            this.packetName = packetName;
            this.timeout = timeout;
            this.deadline = deadline;
            this.admissionBudget = admissionBudget;
            this.onReply = onReply;
            this.onFailure = onFailure;
            this.onAdmissionWaiting = onAdmissionWaiting;
            this.retry = retry;
            this.future = new RequestFuture(this::terminate);
        }

        private synchronized void start() {
            scheduleTimeout();
            active.add(this);
        }

        synchronized CompletableFuture<ZLinkStreamEncodedPayload> enqueueAttempt(
                long sequence, Function<BooleanSupplier, CompletableFuture<Void>> enqueue) {
            if (timeoutTask != null && deadline - nanoTime() > 0) {
                this.sequence = sequence;
                attempts.put(sequence, this);
                write = enqueue.apply(() -> canWrite(sequence));
            }
            return future;
        }

        private synchronized boolean canWrite(long sequence) {
            return this.sequence == sequence && deadline - nanoTime() > 0;
        }

        private boolean admissionRejected(long sequence, String hint, long receivedAt) {
            int millis = admissionDelayMillis(hint);
            long retryAt = 0;
            synchronized (this) {
                if (this.sequence != sequence) {
                    return false;
                }
                if (millis < 0) {
                    terminate();
                } else {
                    clearAttempt();
                    // Later rejections can only produce a later admission deadline.
                    if (admissionBudget < deadline - receivedAt) {
                        deadline = receivedAt + admissionBudget;
                        scheduleTimeout();
                    }
                    retryAt =
                            receivedAt
                                    + TimeUnit.MILLISECONDS.toNanos(
                                            (long) millis + jitterMillis.getAsInt());
                }
            }
            if (millis < 0) {
                publishFailure(
                        ZLinkStreamException.of(
                                ZLinkStreamErrorCode.FRAME_DECODE_FAILED,
                                "Invalid authentication admission retry delay."));
            } else {
                onAdmissionWaiting.accept(Duration.ofMillis(millis));
                scheduleRetry(retryAt);
            }
            return true;
        }

        private void scheduleTimeout() {
            if (timeoutTask != null) {
                timeoutTask.cancel(false);
            }
            long scheduledDeadline = deadline;
            timeoutTask =
                    scheduler.schedule(
                            () -> {
                                synchronized (this) {
                                    if (scheduledDeadline != deadline || !terminate()) {
                                        return;
                                    }
                                }
                                publishFailure(timeoutFailure());
                            },
                            Math.max(0, scheduledDeadline - nanoTime()),
                            TimeUnit.NANOSECONDS);
        }

        private synchronized void scheduleRetry(long retryAt) {
            if (timeoutTask != null && deadline - retryAt > 0) {
                retryTask =
                        scheduler.schedule(
                                () -> {
                                    synchronized (this) {
                                        if (timeoutTask == null || deadline - nanoTime() <= 0) {
                                            return;
                                        }
                                        retryTask = null;
                                    }
                                    retry.accept(this);
                                },
                                Math.max(0, retryAt - nanoTime()),
                                TimeUnit.NANOSECONDS);
            }
        }

        void fail(Throwable failure) {
            if (terminate()) {
                publishFailure(failure);
            }
        }

        private synchronized boolean terminateAttempt(long sequence) {
            return this.sequence == sequence && terminate();
        }

        private synchronized boolean terminate() {
            if (timeoutTask == null) {
                return false;
            }
            active.remove(this);
            var pendingWrite = clearAttempt();
            future.detach();
            if (pendingWrite != null) {
                pendingWrite.cancel(false);
            }
            timeoutTask.cancel(false);
            timeoutTask = null;
            if (retryTask != null) {
                retryTask.cancel(false);
                retryTask = null;
            }
            return true;
        }

        private CompletableFuture<Void> clearAttempt() {
            if (sequence != 0) {
                attempts.remove(sequence, this);
                sequence = 0;
            }
            var pendingWrite = write;
            write = null;
            return pendingWrite;
        }

        private void publishFailure(Throwable failure) {
            onFailure.apply(failure, () -> future.completeExceptionally(failure));
        }

        private ZLinkStreamException timeoutFailure() {
            String message = "request timed out after " + timeout;
            return ZLinkStreamException.of(
                    ZLinkStreamErrorCode.REQUEST_TIMEOUT, message, new TimeoutException(message));
        }
    }

    static final class RequestFuture extends CompletableFuture<ZLinkStreamEncodedPayload> {
        private volatile BooleanSupplier cancelPending;

        RequestFuture(BooleanSupplier cancelPending) {
            this.cancelPending = cancelPending;
        }

        @Override
        public boolean cancel(boolean mayInterruptIfRunning) {
            BooleanSupplier cancel = cancelPending;
            return cancel != null && cancel.getAsBoolean()
                    ? super.cancel(mayInterruptIfRunning)
                    : isCancelled();
        }

        void detach() {
            cancelPending = null;
        }
    }
}
