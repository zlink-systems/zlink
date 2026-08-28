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
             var receiverMonitor = receiver.monitorOpen(
                 config.monitorHwm(), MonitorEventType.CONNECTION_READY);
             var senderMonitor = sender.monitorOpen(
                 config.monitorHwm(), MonitorEventType.CONNECTION_READY)) {
            Duration readyTimeout = Duration.ofMillis(config.connectReadyTimeoutMs());
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
                         List.of(receiver), PollEventFlags.POLLIN);
                     Received received = new Received()) {
                    while (true) {
                        pollSet.poll(-1);
                        boolean stop = false;
                        while (true) {
                            if (!PerfUtil.recvNoWait(receiver, received)) {
                                break;
                            }
                            try {
                                if (received.parts().size() == 1
                                    && PerfStopToken.isStopTokenMessage(received.firstPart())) {
                                    stop = true;
                                    break;
                                }
                                Message payload = PerfUtil.measurementPayload(received.parts());
                                if (payload == null) {
                                    continue;
                                }
                                long receivedNanoTime = System.nanoTime();
                                PerfUtil.Header header = PerfUtil.decodeHeader(
                                    payload, config.size(),
                                    receivedNanoTime);
                                if (header == null) {
                                    continue;
                                }
                                if (header.phase() == PerfUtil.PHASE_ACTIVE
                                    && receivedNanoTime < activeEnd) {
                                    metrics.recordNanos(header.latencyNanos());
                                }
                            } finally {
                                received.close();
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
                        // C parity: raw one-way uses blocking FLAGS_NONE. A
                        // transient failure waits 1ms, then the next loop
                        // writes a fresh timestamp before retrying the sample.
                        while (System.nanoTime() < activeEnd) {
                            active = PerfUtil.resetAndWritePayload(active, config.size(),
                                (byte) PerfUtil.PHASE_ACTIVE, System.nanoTime());
                            if (!trySendBlocking(sender, active)) {
                                PerfUtil.pauseOneWaySendRetry("pair");
                            }
                        }
                    } finally {
                        active.close();
                    }
                    // PERF_SINGLE_TEST_POLICY § 1.4: signal phase end with a
                    // wire-level stop token. The bounded retry helper handles
                    // transient backpressure without losing the terminator.
                    try (PerfSocketPollSet writable = PerfSocketPollSet.fromSockets(
                        List.of(sender), PollEventFlags.POLLOUT)) {
                        writable.setEvents(0);
                        PerfStopToken.sendWithRetry(
                            () -> trySendStop(sender),
                            () -> {
                                writable.setEvents(0, PollEventFlags.POLLOUT);
                                writable.poll(-1);
                            },
                            "pair");
                    }
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

    private static boolean trySendBlocking(PairSocket sender, Message active) {
        try {
            if (PerfUtil.measurementPartCount() == 2) {
                sender.send().message(active).message(PerfUtil.measurementTail())
                    .submit_sync(SendFlags.NONE);
            } else {
                sender.send().message(active).submit_sync(SendFlags.NONE);
            }
            return true;
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

    private static boolean trySendStop(PairSocket sender) {
        try (Message stop = PerfStopToken.newMessage()) {
            sender.send().message(stop).submit_sync(SendFlags.NONE);
            return true;
        } catch (ZlinkSubmitException ex) {
            if (ex.getResult() == SubmitResult.BACKPRESSURED) {
                return false;
            }
            throw ex;
        } catch (ZlinkException ex) {
            if (PerfErrno.isRetryableSend(ex.getNativeErrno())) {
                return false;
            }
            throw ex;
        }
    }
}
