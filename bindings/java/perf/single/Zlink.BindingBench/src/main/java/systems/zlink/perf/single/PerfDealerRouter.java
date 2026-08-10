/* SPDX-License-Identifier: MPL-2.0 */

package systems.zlink.perf.single;

import systems.zlink.contracts.core.Context;
import systems.zlink.contracts.sockets.DealerSocket;
import systems.zlink.contracts.messaging.Message;
import systems.zlink.contracts.eventing.MonitorEventType;
import systems.zlink.contracts.eventing.PollEventFlags;
import systems.zlink.contracts.messaging.Received;
import systems.zlink.contracts.sockets.RouterSocket;
import systems.zlink.contracts.sockets.SendFlags;
import systems.zlink.contracts.sockets.SocketType;
import systems.zlink.contracts.errors.ZlinkSubmitException;
import systems.zlink.contracts.sockets.SubmitResult;
import systems.zlink.contracts.errors.ZlinkException;
import systems.zlink.perf.PerfSocketPollSet;
import systems.zlink.perf.PerfStopToken;
import systems.zlink.perf.PerfErrno;
import systems.zlink.perf.PerfUtil;
import java.time.Duration;
import java.util.List;
import java.util.concurrent.atomic.AtomicBoolean;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.atomic.AtomicReference;

final class PerfDealerRouter {
    private static final MonitorEventType READY_EVENT = MonitorEventType.CONNECTION_READY;

    private PerfDealerRouter() {
    }

    static PerfUtil.Result run(PerfUtil.Config config) {
        String endpoint = PerfUtil.endpoint(config.transport(), "single-dealer-router");
        CountDownLatch finished = new CountDownLatch(1);
        CountDownLatch routed = new CountDownLatch(1);
        AtomicBoolean probePending = new AtomicBoolean(true);
        AtomicReference<Throwable> failure = new AtomicReference<>();
        PerfUtil.Metrics metrics = new PerfUtil.Metrics(config);
        try (Context ctx = PerfUtil.newContext(config);
             RouterSocket receiver = ctx.createRouterSocket();
             DealerSocket sender = ctx.createDealerSocket();
             var receiverMonitor = receiver.monitorOpen(MonitorEventType.CONNECTION_READY);
             var senderMonitor = sender.monitorOpen(MonitorEventType.CONNECTION_READY)) {
            Duration readyTimeout = Duration.ofMillis(config.connectReadyTimeoutMs());
            PerfUtil.applyMonitorOptions(receiverMonitor, config);
            PerfUtil.applyMonitorOptions(senderMonitor, config);
            PerfUtil.applySocketOptions(receiver, config);
            PerfUtil.applySocketOptions(sender, config);
            PerfUtil.configureServerTls(receiver, config.transport());
            PerfUtil.configureClientTls(sender, config.transport());
            receiver.bind(PerfUtil.bindEndpoint(endpoint, config.transport()));
            sender.connect(PerfUtil.connectedEndpoint(receiver, endpoint,
                config.transport()));
            PerfUtil.waitForMonitorEventWithActivity(receiverMonitor, receiver,
                READY_EVENT, 1, readyTimeout, "dealer/router receiver ready");
            PerfUtil.waitForMonitorEvent(senderMonitor, READY_EVENT, 1,
                readyTimeout, "dealer/router sender ready");
            PerfUtil.recalculateAutoHwm(ctx);

            // PERF_SINGLE_TEST_POLICY § 1.4: receiver waits with -1 and exits
            // on wire-level stop token. The probe still uses a header phase
            // (PHASE_WARMUP) since it is part of the ready barrier, not a
            // shutdown signal.
            long activeEnd = System.nanoTime()
                + config.durationSeconds() * 1_000_000_000L;
            Thread receiverThread = new Thread(() -> {
                try (PerfSocketPollSet pollSet = PerfSocketPollSet.fromSockets(
                    List.of(receiver), PollEventFlags.POLLIN)) {
                    while (true) {
                        pollSet.poll(-1);
                        boolean stop = false;
                        while (true) {
                            systems.zlink.contracts.messaging.Received received =
                                PerfUtil.recvNoWait(receiver);
                            if (received == null) {
                                break;
                            }
                            try (received) {
                                if (PerfStopToken.isStopTokenMessage(received.firstPart())) {
                                    stop = true;
                                    break;
                                }
                                PerfUtil.Header header = PerfUtil.decodeHeader(
                                    received.firstPart(), config.size());
                                if (header == null) {
                                    continue;
                                }
                                if (header.phase() == PerfUtil.PHASE_WARMUP
                                    && probePending.compareAndSet(true, false)) {
                                    routed.countDown();
                                    continue;
                                }
                                if (header.phase() == PerfUtil.PHASE_ACTIVE
                                    && System.nanoTime() < activeEnd) {
                                    metrics.recordNanos(header.latencyNanos());
                                }
                            }
                        }
                        if (stop) {
                            finished.countDown();
                            return;
                        }
                    }
                } catch (Throwable ex) {
                    failure.compareAndSet(null, ex);
                    finished.countDown();
                }
            }, "single-dealer-router-receiver");
            receiverThread.start();

            long probeDeadline = System.nanoTime() + Duration.ofSeconds(10).toNanos();
            while (System.nanoTime() < probeDeadline && routed.getCount() != 0) {
                try (var probe = PerfUtil.payload(config.size(),
                         (byte) PerfUtil.PHASE_WARMUP, System.nanoTime())) {
                    if (!trySendActive(sender, probe)) {
                        Thread.onSpinWait();
                    }
                }
                try {
                    if (routed.await(10, java.util.concurrent.TimeUnit.MILLISECONDS)) {
                        break;
                    }
                } catch (InterruptedException ex) {
                    Thread.currentThread().interrupt();
                    throw new IllegalStateException(
                        "dealer/router self-check interrupted", ex);
                }
            }
            PerfUtil.await(routed, "dealer/router self-check",
                Duration.ofSeconds(10));

            Thread traffic = new Thread(() -> {
                try {
                    Message active = PerfUtil.payloadTemplate(config.size());
                    try {
                        while (System.nanoTime() < activeEnd) {
                            active = PerfUtil.resetAndWritePayload(active, config.size(),
                                (byte) PerfUtil.PHASE_ACTIVE, System.nanoTime());
                            while (System.nanoTime() < activeEnd
                                && !trySendBlocking(sender, active)) {
                                Thread.onSpinWait();
                            }
                        }
                    } finally {
                        active.close();
                    }
                    // PERF_SINGLE_TEST_POLICY § 1.4: signal phase end with a
                    // wire-level stop token. C parity:
                    // perf_dealer_router.cpp send_dealer_stop_token (~165-183)
                    // / perf_single_one_way.hpp send_stop_token_with_retry
                    // bounded-retries through transient backpressure so the
                    // ROUTER receiver always observes the terminator.
                    PerfStopToken.sendWithRetry(() -> {
                        try (Message stop = PerfStopToken.newMessage()) {
                            return sender.send()
                                .message(stop)
                                .flags(SendFlags.NONE)
                                .submit();
                        }
                    }, "dealer/router");
                } catch (Throwable ex) {
                    failure.compareAndSet(null, ex);
                    finished.countDown();
                }
            }, "single-dealer-router-sender");
            traffic.start();
            PerfUtil.await(finished, "dealer/router receiver",
                Duration.ofSeconds(config.durationSeconds() + 10L));
            PerfUtil.join(traffic, "dealer/router sender", Duration.ofSeconds(10));
            PerfUtil.join(receiverThread, "dealer/router receiver thread",
                Duration.ofSeconds(10));
            if (failure.get() != null) {
                throw new IllegalStateException("dealer/router receiver failed",
                    failure.get());
            }
            PerfUtil.printSingleMonitorAutoHwm(config, receiverMonitor, "receiver",
                SocketType.ROUTER);
            PerfUtil.printSingleMonitorAutoHwm(config, senderMonitor, "sender",
                SocketType.DEALER);
            return metrics.finishSingle(config);
        }
    }

    private static boolean trySendActive(DealerSocket sender, Message active) {
        try {
            return sender.send()
                .message(active)
                .flags(SendFlags.DONT_WAIT)
                .submit();
        } catch (systems.zlink.contracts.errors.ZlinkSubmitException ex) {
            if (ex.getResult()
                == systems.zlink.contracts.sockets.SubmitResult.BACKPRESSURED) {
                return false;
            }
            throw ex;
        } catch (systems.zlink.contracts.errors.ZlinkException ex) {
            int errno = ex.getNativeErrno();
            if (PerfErrno.isRetryableSend(errno)) {
                return false;
            }
            throw ex;
        }
    }

    private static boolean trySendBlocking(DealerSocket sender, Message active) {
        try {
            return sender.send()
                .message(active)
                .flags(SendFlags.NONE)
                .submit();
        } catch (systems.zlink.contracts.errors.ZlinkSubmitException ex) {
            if (ex.getResult()
                == systems.zlink.contracts.sockets.SubmitResult.BACKPRESSURED) {
                return false;
            }
            throw ex;
        } catch (systems.zlink.contracts.errors.ZlinkException ex) {
            int errno = ex.getNativeErrno();
            if (PerfErrno.isRetryableSend(errno)
                || errno == PerfErrno.ETIMEDOUT) {
                return false;
            }
            throw ex;
        }
    }
}
