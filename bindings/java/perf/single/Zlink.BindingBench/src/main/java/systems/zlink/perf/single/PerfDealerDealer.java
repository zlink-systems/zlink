/* SPDX-License-Identifier: MPL-2.0 */

package systems.zlink.perf.single;

import systems.zlink.contracts.core.Context;
import systems.zlink.contracts.sockets.DealerSocket;
import systems.zlink.contracts.messaging.Message;
import systems.zlink.contracts.eventing.MonitorEventType;
import systems.zlink.contracts.eventing.PollEventFlags;
import systems.zlink.contracts.messaging.Received;
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
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.atomic.AtomicReference;

final class PerfDealerDealer {
    private static final MonitorEventType READY_EVENT = MonitorEventType.CONNECTION_READY;

    private PerfDealerDealer() {
    }

    static PerfUtil.Result run(PerfUtil.Config config) {
        String endpoint = PerfUtil.endpoint(config.transport(), "single-dealer-dealer");
        CountDownLatch finished = new CountDownLatch(1);
        AtomicReference<Throwable> failure = new AtomicReference<>();
        PerfUtil.Metrics metrics = new PerfUtil.Metrics(config);
        try (Context ctx = PerfUtil.newContext(config);
             DealerSocket receiver = ctx.createDealerSocket();
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
            PerfUtil.waitForMonitorEvent(senderMonitor, READY_EVENT, 1,
                readyTimeout, "dealer/dealer sender ready");
            PerfUtil.waitForMonitorEvent(receiverMonitor, READY_EVENT, 1,
                readyTimeout, "dealer/dealer receiver ready");
            PerfUtil.recalculateAutoHwm(ctx);

            // PERF_SINGLE_TEST_POLICY § 1.4: receiver waits with -1 (signal-driven)
            // and exits on wire-level stop token instead of an atomic + short
            // polling fallback.
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
            }, "single-dealer-dealer-receiver");
            receiverThread.start();

            Thread traffic = new Thread(() -> {
                try {
                    Message active = PerfUtil.payloadTemplate(config.size());
                    try {
                        // C parity: perf_dealer_dealer.cpp send step uses
                        // send_socket_active_message(sender, &part,
                        // ZLINK_DONTWAIT, true) and perf_single_one_way.hpp
                        // send_active_samples (~169-196) retries the same
                        // sample (continue, no advance) on transient
                        // backpressure. A blocking submit() can wedge the
                        // sender on a full HWM so the active loop never
                        // reaches activeEnd and the wire stop token below is
                        // never emitted, hanging the receiver's poll(-1) until
                        // the harness timeout. Mirror C: nonblocking send,
                        // retry on transient backpressure until the deadline.
                        while (System.nanoTime() < activeEnd) {
                            active = PerfUtil.resetAndWritePayload(active, config.size(),
                                (byte) PerfUtil.PHASE_ACTIVE, System.nanoTime());
                            while (System.nanoTime() < activeEnd
                                && !trySendActive(sender, active)) {
                                // send_step_retry: re-attempt same sample.
                            }
                        }
                    } finally {
                        active.close();
                    }
                    // PERF_SINGLE_TEST_POLICY § 1.4: signal phase end with a
                    // wire-level stop token. C parity:
                    // perf_single_one_way.hpp send_stop_token_with_retry
                    // (~200-235) bounded-retries the non-blocking send through
                    // transient backpressure so the receiver always observes
                    // the terminator.
                    PerfStopToken.sendWithRetry(() -> {
                        try (Message stop = PerfStopToken.newMessage()) {
                            return sender.send()
                                .message(stop)
                                .flags(SendFlags.DONT_WAIT)
                                .submit();
                        }
                    }, "dealer/dealer");
                } catch (Throwable ex) {
                    failure.compareAndSet(null, ex);
                    finished.countDown();
                }
            }, "single-dealer-dealer-sender");
            traffic.start();
            PerfUtil.await(finished, "dealer/dealer receiver",
                Duration.ofSeconds(config.durationSeconds() + 10L));
            PerfUtil.join(traffic, "dealer/dealer sender", Duration.ofSeconds(10));
            PerfUtil.join(receiverThread, "dealer/dealer receiver thread",
                Duration.ofSeconds(10));
            if (failure.get() != null) {
                throw new IllegalStateException("dealer/dealer receiver failed",
                    failure.get());
            }
            PerfUtil.printSingleMonitorAutoHwm(config, receiverMonitor, "receiver",
                SocketType.DEALER);
            PerfUtil.printSingleMonitorAutoHwm(config, senderMonitor, "sender",
                SocketType.DEALER);
            return metrics.finishSingle(config);
        }
    }

    // C parity: perf_single_one_way.hpp send_socket_active_message
    // (~145-166). Nonblocking send; true == send_step_sent. Transient
    // backpressure (BACKPRESSURED / EAGAIN / EINTR) -> false so the caller
    // re-attempts the same sample (send_step_retry); a non-transient error
    // is fatal and propagates (send_step_fatal).
    private static boolean trySendActive(DealerSocket sender, Message active) {
        try (Message outbound = Message.from(active)) {
            return sender.send()
                .message(outbound)
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
}
