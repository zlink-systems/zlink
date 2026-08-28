/* SPDX-License-Identifier: MPL-2.0 */

package systems.zlink.perf.multi;

import systems.zlink.contracts.core.Context;
import systems.zlink.contracts.messaging.Message;
import systems.zlink.contracts.core.RoutingId;
import systems.zlink.contracts.sockets.SocketType;
import systems.zlink.contracts.sockets.StreamSocket;
import systems.zlink.perf.PerfControl;
import systems.zlink.perf.PerfStopToken;
import systems.zlink.perf.PerfUtil;
import java.io.BufferedReader;
import java.io.InputStreamReader;
import java.nio.charset.StandardCharsets;
import java.time.Duration;
import java.util.ArrayDeque;
import java.util.ArrayList;
import java.util.Deque;
import java.util.HashSet;
import java.util.List;
import java.util.Objects;
import java.util.Set;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import java.util.concurrent.atomic.AtomicBoolean;
import java.util.concurrent.atomic.AtomicReference;

final class PerfMultiStream {
    private PerfMultiStream() {
    }

    static PerfUtil.Result runServer(PerfUtil.Config config) {
        AtomicBoolean stopRequested = new AtomicBoolean(false);
        AtomicReference<Throwable> failure = new AtomicReference<>();
        Object stopSignal = new Object();
        Thread controlWatcher = null;

        try (Context ctx = PerfUtil.newContext(config);
            StreamSocket server = ctx.createStreamSocket()) {
            PerfUtil.applySocketOptions(server, config);
            PerfUtil.configureServerTls(server, config.transport());
            Duration streamTimeout = Duration.ofMillis(streamTimeoutMs());
            server.options().sendTimeout(streamTimeout);
            server.options().recvTimeout(streamTimeout);
            Duration drainTimeout = streamDrainTimeout(streamTimeout);

            try (StreamReplyDispatcher dispatcher =
                     new StreamReplyDispatcher(server, streamTimeout,
                         drainTimeout, stopRequested, stopSignal, failure)) {
                controlWatcher = startControlWatcher(dispatcher);
                server.bind(config.endpoint());
                PerfUtil.recalculateAutoHwm(ctx);
                PerfUtil.printMultiSocketAutoHwm(config, server, "server",
                    "server", SocketType.STREAM);
                server.onPacket(dispatcher::onPacket);
                PerfControl.emitReady(config.endpoint());

                awaitStop(stopRequested, stopSignal);
                dispatcher.stopAndDrain();
                Throwable error = failure.get();
                if (error != null) {
                    throw new IllegalStateException(
                        "stream reply failed", error);
                }
            }
            return PerfUtil.Result.silent(config);
        } finally {
            stopRequested.set(true);
            signal(stopSignal);
            if (controlWatcher != null) {
                controlWatcher.interrupt();
            }
        }
    }

    static PerfUtil.Result runClient(PerfUtil.Config config) {
        throw new IllegalStateException("MULTI_STREAM requires the shared raw stream client");
    }

    private static Thread startControlWatcher(
        StreamReplyDispatcher dispatcher) {
        Thread watcher = new Thread(() -> {
            try (BufferedReader reader = new BufferedReader(
                new InputStreamReader(System.in, StandardCharsets.UTF_8))) {
                String line;
                while ((line = reader.readLine()) != null) {
                    if ("STOP".equals(line) || "QUIT".equals(line)) {
                        dispatcher.requestStop();
                        return;
                    }
                }
            } catch (Exception ex) {
                throw new IllegalStateException("stream control watcher failed", ex);
            }
        }, "stream-control");
        watcher.setDaemon(true);
        watcher.start();
        return watcher;
    }

    // C parity: write the framing prefix and the received native frames into
    // the outbound native Message. The public Message API copies native-to-
    // native, so this avoids staging the complete packet in a Java byte[].
    private static Message buildPacketFrame(Message header, Message body) {
        int headerSize = header.size();
        int bodySize = body.size();
        int total = 6 + headerSize + bodySize;
        Message packet = Message.allocate(total);
        packet.writeShortBe(0, (short) headerSize);
        packet.writeIntBe(2, bodySize);
        packet.copyFrom(header, 0, 6, headerSize);
        packet.copyFrom(body, 0, 6 + headerSize, bodySize);
        return packet;
    }

    private static void awaitStop(AtomicBoolean stopRequested,
                                  Object stopSignal) {
        synchronized (stopSignal) {
            while (!stopRequested.get()) {
                try {
                    stopSignal.wait(50L);
                } catch (InterruptedException error) {
                    Thread.currentThread().interrupt();
                    throw new IllegalStateException(
                        "stream stop wait interrupted", error);
                }
            }
        }
    }

    private static void signal(Object stopSignal) {
        synchronized (stopSignal) {
            stopSignal.notifyAll();
        }
    }

    private static int streamTimeoutMs() {
        String configured = System.getenv("PERF_STREAM_TIMEOUT_MS");
        if (configured == null || configured.isBlank()) {
            return 5_000;
        }
        try {
            int value = Integer.parseInt(configured);
            return value >= 0 ? value : 5_000;
        } catch (NumberFormatException ignored) {
            return 5_000;
        }
    }

    private static Duration streamDrainTimeout(Duration streamTimeout) {
        long timeoutMs = Math.max(1_000L,
            Math.multiplyExact(streamTimeout.toMillis(), 4L));
        return Duration.ofMillis(timeoutMs);
    }

    /**
     * Owns callback-copied packets and starts their public async terminals on
     * an application thread. The queue and pending set are tracking only:
     * neither imposes an application-level admission window.
     */
    static final class StreamReplyDispatcher implements AutoCloseable {
        private final StreamSocket server;
        private final Duration sendTimeout;
        private final long drainTimeoutNanos;
        private final AtomicBoolean stopRequested;
        private final Object stopSignal;
        private final AtomicReference<Throwable> failure;
        private final Object lock = new Object();
        private final Deque<PendingPacket> queue = new ArrayDeque<>();
        private final Set<CompletableFuture<Void>> pending =
            new HashSet<>();
        private final Thread worker;

        private boolean accepting = true;
        private long drainDeadlineNanos = Long.MAX_VALUE;

        StreamReplyDispatcher(StreamSocket server,
                              Duration sendTimeout,
                              Duration drainTimeout,
                              AtomicBoolean stopRequested,
                              Object stopSignal,
                              AtomicReference<Throwable> failure) {
            this.server = Objects.requireNonNull(server, "server");
            this.sendTimeout = Objects.requireNonNull(
                sendTimeout, "sendTimeout");
            this.drainTimeoutNanos = drainTimeout.toNanos();
            this.stopRequested = Objects.requireNonNull(
                stopRequested, "stopRequested");
            this.stopSignal = Objects.requireNonNull(
                stopSignal, "stopSignal");
            this.failure = Objects.requireNonNull(failure, "failure");
            worker = new Thread(this::run, "stream-reply-dispatcher");
            worker.setDaemon(true);
            worker.start();
        }

        void onPacket(RoutingId routingId, Message header, Message body) {
            if (routingId == null || stopRequested.get()) {
                return;
            }
            if (PerfStopToken.isStopTokenMessage(body)) {
                requestStop();
                return;
            }

            Message packet = null;
            try {
                RoutingId ownedRoutingId = RoutingId.from(
                    routingId.toBytes());
                packet = buildPacketFrame(header, body);
                if (enqueue(new PendingPacket(ownedRoutingId, packet))) {
                    packet = null;
                }
            } catch (RuntimeException | Error error) {
                recordFailure(error);
            } finally {
                if (packet != null) {
                    packet.close();
                }
            }
        }

        void requestStop() {
            synchronized (lock) {
                if (accepting) {
                    accepting = false;
                    drainDeadlineNanos = saturatingAdd(
                        System.nanoTime(), drainTimeoutNanos);
                }
                lock.notifyAll();
            }
            stopRequested.set(true);
            signal(stopSignal);
        }

        void stopAndDrain() {
            requestStop();
            long deadline;
            synchronized (lock) {
                deadline = drainDeadlineNanos;
            }
            joinUntil(deadline);
            if (worker.isAlive()) {
                worker.interrupt();
                recordFailure(new IllegalStateException(
                    "stream reply dispatcher drain timed out"));
            }
            awaitPendingUntil(deadline);
            cancelPending();
            dropQueued();
        }

        @Override
        public void close() {
            stopAndDrain();
        }

        private boolean enqueue(PendingPacket packet) {
            synchronized (lock) {
                if (!accepting || stopRequested.get()) {
                    return false;
                }
                queue.addLast(packet);
                lock.notifyAll();
                return true;
            }
        }

        private void run() {
            for (;;) {
                PendingPacket packet = takeNext();
                if (packet == null) {
                    return;
                }
                send(packet);
                if (failure.get() != null) {
                    dropQueued();
                    return;
                }
            }
        }

        private PendingPacket takeNext() {
            synchronized (lock) {
                for (;;) {
                    if (!accepting
                        && System.nanoTime() >= drainDeadlineNanos) {
                        dropQueuedLocked();
                        lock.notifyAll();
                        return null;
                    }
                    if (!queue.isEmpty()) {
                        return queue.removeFirst();
                    }
                    if (!accepting) {
                        lock.notifyAll();
                        return null;
                    }
                    try {
                        lock.wait();
                    } catch (InterruptedException error) {
                        Thread.currentThread().interrupt();
                        recordFailure(error);
                        return null;
                    }
                }
            }
        }

        private void send(PendingPacket queued) {
            CompletionStage<Void> stage;
            try (Message packet = queued.message()) {
                stage = server.sendAsync(queued.routingId())
                    .message(packet)
                    .timeout(sendTimeout)
                    .submit();
            } catch (RuntimeException | Error error) {
                if (!PerfMultiRoutedRelay.isStaleRoute(error)) {
                    recordFailure(error);
                }
                return;
            }

            CompletableFuture<Void> future = Objects.requireNonNull(stage,
                "stream async submit stage").toCompletableFuture();
            synchronized (lock) {
                pending.add(future);
            }
            future.whenComplete((ignored, error) ->
                onSendComplete(future, error));
        }

        private void onSendComplete(CompletableFuture<Void> future,
                                    Throwable error) {
            if (error != null && !future.isCancelled()) {
                Throwable cause =
                    PerfMultiAsyncSendLoop.completionCause(error);
                if (!PerfMultiRoutedRelay.isStaleRoute(cause)) {
                    recordFailure(cause);
                }
            }
            synchronized (lock) {
                pending.remove(future);
                lock.notifyAll();
            }
        }

        private void recordFailure(Throwable error) {
            Throwable cause = PerfMultiAsyncSendLoop.completionCause(error);
            if (failure.compareAndSet(null, cause)) {
                requestStop();
            }
        }

        private void dropQueued() {
            synchronized (lock) {
                dropQueuedLocked();
                lock.notifyAll();
            }
        }

        private void dropQueuedLocked() {
            while (!queue.isEmpty()) {
                queue.removeFirst().message().close();
            }
        }

        private void awaitPendingUntil(long deadlineNanos) {
            synchronized (lock) {
                while (!pending.isEmpty()) {
                    long remaining = deadlineNanos - System.nanoTime();
                    if (remaining <= 0L) {
                        return;
                    }
                    try {
                        waitRemaining(lock, remaining);
                    } catch (InterruptedException error) {
                        Thread.currentThread().interrupt();
                        recordFailure(error);
                        return;
                    }
                }
            }
        }

        private void cancelPending() {
            List<CompletableFuture<Void>> tail;
            synchronized (lock) {
                if (pending.isEmpty()) {
                    return;
                }
                tail = new ArrayList<>(pending);
                pending.clear();
            }
            for (CompletableFuture<Void> future : tail) {
                future.cancel(true);
            }
        }

        private void joinUntil(long deadlineNanos) {
            while (worker.isAlive()) {
                long remaining = deadlineNanos - System.nanoTime();
                if (remaining <= 0) {
                    return;
                }
                long millis = Math.max(1L,
                    Math.min(Integer.MAX_VALUE,
                        (remaining + 999_999L) / 1_000_000L));
                try {
                    worker.join(millis);
                } catch (InterruptedException error) {
                    Thread.currentThread().interrupt();
                    recordFailure(error);
                    return;
                }
            }
        }

        private static void waitRemaining(Object lock, long remainingNanos)
            throws InterruptedException {
            long millis = remainingNanos / 1_000_000L;
            int nanos = (int) (remainingNanos % 1_000_000L);
            lock.wait(millis, nanos);
        }

        private static long saturatingAdd(long left, long right) {
            if (right > 0L && left > Long.MAX_VALUE - right) {
                return Long.MAX_VALUE;
            }
            return left + right;
        }
    }

    private record PendingPacket(RoutingId routingId, Message message) {
    }

}
