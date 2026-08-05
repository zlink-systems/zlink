/* SPDX-License-Identifier: MPL-2.0 */

package systems.zlink.perf.single;

import systems.zlink.contracts.core.Context;
import systems.zlink.contracts.messaging.Message;
import systems.zlink.contracts.eventing.MonitorEventType;
import systems.zlink.contracts.sockets.PairSocket;
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

final class PerfPair {
    private static final MonitorEventType READY_EVENT = MonitorEventType.CONNECTION_READY;

    private PerfPair() {
    }

    static PerfUtil.Result run(PerfUtil.Config config) {
        String endpoint = PerfUtil.endpoint(config.transport(), "single-pair");
        CountDownLatch finished = new CountDownLatch(1);
        AtomicReference<Throwable> failure = new AtomicReference<>();
        PerfUtil.Metrics metrics = new PerfUtil.Metrics(config);
        try (Context ctx = PerfUtil.newContext(config);
             PairSocket receiver = ctx.createPairSocket();
             PairSocket sender = ctx.createPairSocket();
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
                readyTimeout, "pair sender ready");
            PerfUtil.waitForMonitorEvent(receiverMonitor, READY_EVENT, 1,
                readyTimeout, "pair receiver ready");
            PerfUtil.recalculateAutoHwm(ctx);

            // PERF_SINGLE_TEST_POLICY § 1.4: receiver waits with -1 and exits
            // on wire-level stop token; no idle-drain timer fallback.
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
            }, "single-pair-receiver");
            receiverThread.start();

            Thread traffic = new Thread(() -> {
                try {
                    Message active = PerfUtil.payloadTemplate(config.size());
                    try {
                        // C parity: perf_pair.cpp send step uses
                        // send_socket_active_message(sender, &part,
                        // ZLINK_DONTWAIT, true) and perf_single_one_way.hpp
                        // send_active_samples (~169-196) retries (continue,
                        // no seq advance) on transient backpressure. A
                        // blocking submit() here can wedge the sender thread
                        // on a full PAIR HWM, so the active loop never reaches
                        // activeEnd, the wire stop token below is never sent,
                        // and the receiver's poll(-1) blocks until the harness
                        // timeout (pair_receiver_thread_timed_out). Mirror C:
                        // nonblocking send, retry on transient backpressure
                        // until the duration deadline.
                        while (System.nanoTime() < activeEnd) {
                            active = PerfUtil.resetAndWritePayload(active, config.size(),
                                (byte) PerfUtil.PHASE_ACTIVE, System.nanoTime());
                            while (System.nanoTime() < activeEnd
                                && !trySendActive(sender, active)) {
                                // send_step_retry: re-attempt the same sample
                                // without advancing (C send_active_samples
                                // `continue`).
                            }
                        }
                    } finally {
                        active.close();
                    }
                    // PERF_SINGLE_TEST_POLICY § 1.4: signal phase end with a
                    // wire-level stop token. C parity:
                    // perf_single_one_way.hpp send_stop_token_with_retry
                    // (~200-235) bounded-retries through transient
                    // backpressure so the receiver always observes the
                    // terminator (a single submit can lose it under load,
                    // hanging the receiver on poll(-1)).
                    PerfStopToken.sendWithRetry(() -> {
                        try (Message stop = PerfStopToken.newMessage()) {
                            return sender.send()
                                .message(stop)
                                .flags(SendFlags.DONT_WAIT)
                                .submit();
                        }
                    }, "pair");
                } catch (Throwable ex) {
                    failure.compareAndSet(null, ex);
                    finished.countDown();
                }
            }, "single-pair-sender");
            traffic.start();
            PerfUtil.await(finished, "pair receiver",
                Duration.ofSeconds(config.durationSeconds() + 10L));
            PerfUtil.join(traffic, "pair sender", Duration.ofSeconds(10));
            PerfUtil.join(receiverThread, "pair receiver thread",
                Duration.ofSeconds(10));
            if (failure.get() != null) {
                throw new IllegalStateException("pair sender failed", failure.get());
            }
            PerfUtil.printSingleMonitorAutoHwm(config, receiverMonitor, "receiver",
                SocketType.PAIR);
            PerfUtil.printSingleMonitorAutoHwm(config, senderMonitor, "sender",
                SocketType.PAIR);
            return metrics.finishSingle(config);
        }
    }

    // C parity: perf_single_one_way.hpp send_socket_active_message
    // (~145-166). Nonblocking send; returns true when the sample was
    // accepted (send_step_sent). Transient backpressure (BACKPRESSURED /
    // EAGAIN / EINTR) yields false so the caller re-attempts the same
    // sample without advancing (send_step_retry). A non-transient failure
    // is fatal and propagates (send_step_fatal).
    private static boolean trySendActive(PairSocket sender, Message active) {
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
