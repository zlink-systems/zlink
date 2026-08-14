/* SPDX-License-Identifier: MPL-2.0 */

package systems.zlink.perf.single;

import systems.zlink.contracts.core.Context;
import systems.zlink.contracts.messaging.Message;
import systems.zlink.contracts.eventing.MonitorEventType;
import systems.zlink.contracts.eventing.PollEventFlags;
import systems.zlink.contracts.messaging.Received;
import systems.zlink.contracts.errors.ZlinkRecvException;
import systems.zlink.contracts.sockets.RecvFlags;
import systems.zlink.contracts.sockets.RecvResult;
import systems.zlink.contracts.sockets.RouterSocket;
import systems.zlink.contracts.core.RoutingId;
import systems.zlink.contracts.sockets.SocketType;
import systems.zlink.contracts.errors.ZlinkSubmitException;
import systems.zlink.contracts.sockets.SubmitResult;
import systems.zlink.contracts.errors.ZlinkException;
import systems.zlink.perf.PerfSocketPollSet;
import systems.zlink.perf.PerfStopToken;
import systems.zlink.perf.PerfErrno;
import systems.zlink.perf.PerfUtil;
import java.util.Arrays;
import java.nio.charset.StandardCharsets;
import java.time.Duration;
import java.util.List;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.atomic.AtomicBoolean;
import java.util.concurrent.atomic.AtomicReference;

final class PerfRouterRouter {
    private static final MonitorEventType READY_EVENT = MonitorEventType.CONNECTION_READY;
    private static final RoutingId ROUTER1 = RoutingId.from(
        "ROUTER1".getBytes(StandardCharsets.UTF_8));
    private static final RoutingId ROUTER2 = RoutingId.from(
        "ROUTER2".getBytes(StandardCharsets.UTF_8));
    private static final byte[] PING = "PING".getBytes(StandardCharsets.UTF_8);
    private static final byte[] PONG = "PONG".getBytes(StandardCharsets.UTF_8);

    private PerfRouterRouter() {
    }

    static PerfUtil.Result run(PerfUtil.Config config) {
        String endpoint = PerfUtil.endpoint(config.transport(), "single-router-router");
        CountDownLatch finished = new CountDownLatch(1);
        CountDownLatch routed = new CountDownLatch(1);
        AtomicBoolean probePending = new AtomicBoolean(true);
        AtomicReference<Throwable> failure = new AtomicReference<>();
        PerfUtil.Metrics metrics = new PerfUtil.Metrics(config);
        boolean sharedContext = true;
        Context receiverCtx = PerfUtil.newContext(config);
        Context senderCtx = sharedContext ? receiverCtx : PerfUtil.newContext(config);
        try (RouterSocket receiver = receiverCtx.createRouterSocket();
             RouterSocket sender = senderCtx.createRouterSocket();
             var receiverMonitor = receiver.monitorOpen(MonitorEventType.CONNECTION_READY);
             var senderMonitor = sender.monitorOpen(MonitorEventType.CONNECTION_READY)) {
            Duration readyTimeout = Duration.ofMillis(config.connectReadyTimeoutMs());
            PerfUtil.applyMonitorOptions(receiverMonitor, config);
            PerfUtil.applyMonitorOptions(senderMonitor, config);
            PerfUtil.applySocketOptions(receiver, config);
            PerfUtil.applySocketOptions(sender, config);
            PerfUtil.recalculateAutoHwm(receiverCtx);
            PerfUtil.configureServerTls(receiver, config.transport());
            PerfUtil.configureClientTls(sender, config.transport());
            receiver.setRoutingId(ROUTER1);
            sender.setRoutingId(ROUTER2);
            receiver.options().mandatory(true);
            sender.options().mandatory(true);
            sender.options().setConnectRoutingId(ROUTER1);
            receiver.bind(PerfUtil.bindEndpoint(endpoint, config.transport()));
            sender.connect(PerfUtil.connectedEndpoint(receiver, endpoint,
                config.transport()));
            PerfUtil.waitForMonitorEventWithActivity(receiverMonitor, receiver,
                READY_EVENT, 1, readyTimeout, "router/router receiver ready");
            PerfUtil.waitForMonitorEventWithActivity(senderMonitor, sender,
                READY_EVENT, 1, readyTimeout, "router/router sender ready");
            RoutingId targetRoute = performRouterRouterHandshake(receiver, sender,
                ROUTER1, ROUTER2,
                Duration.ofMillis(config.connectReadyTimeoutMs()));
            drainRouterSocket(receiver);
            drainRouterSocket(sender);

            // PERF_SINGLE_TEST_POLICY § 1.4: receiver waits with -1 and exits
            // on wire-level stop token.
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
                                if (PerfStopToken.isStopTokenMessage(received.firstPart())) {
                                    stop = true;
                                    break;
                                }
                                long receivedNanoTime = System.nanoTime();
                                PerfUtil.Header header = PerfUtil.decodeHeader(
                                    received.firstPart(), config.size(),
                                    receivedNanoTime);
                                if (header == null) {
                                    continue;
                                }
                                if (header.phase() == PerfUtil.PHASE_WARMUP
                                    && probePending.compareAndSet(true, false)) {
                                    routed.countDown();
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
            }, "single-router-router-receiver");
            receiverThread.start();

            long probeDeadline = System.nanoTime() + Duration.ofSeconds(10).toNanos();
            while (System.nanoTime() < probeDeadline && routed.getCount() != 0) {
                try (Message probe = PerfUtil.payload(config.size(),
                         (byte) PerfUtil.PHASE_WARMUP, System.nanoTime())) {
                    if (!trySendActive(sender, targetRoute, probe)) {
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
                        "router/router self-check interrupted", ex);
                }
            }
            PerfUtil.await(routed, "router/router self-check", Duration.ofSeconds(10));

            Thread traffic = new Thread(() -> {
                try {
                    Message active = PerfUtil.payloadTemplate(config.size());
                    try {
                        while (System.nanoTime() < activeEnd) {
                            active = PerfUtil.resetAndWritePayload(active, config.size(),
                                (byte) PerfUtil.PHASE_ACTIVE, System.nanoTime());
                            if (!trySendBlocking(sender, targetRoute, active)) {
                                PerfUtil.pauseOneWaySendRetry("router/router");
                            }
                        }
                    } finally {
                        active.close();
                    }
                    // PERF_SINGLE_TEST_POLICY § 1.4: signal phase end with a
                    // wire-level stop token routed via the handshaken target
                    // route. ROUTER->ROUTER requires an explicit routing id.
                    sendRouterStopToken(sender, targetRoute);
                } catch (Throwable ex) {
                    failure.compareAndSet(null, ex);
                    finished.countDown();
                }
            }, "single-router-router-sender");
            traffic.start();
            PerfUtil.await(finished, "router/router receiver",
                Duration.ofSeconds(config.durationSeconds() + 30L));
            PerfUtil.join(traffic, "router/router sender", Duration.ofSeconds(10));
            PerfUtil.join(receiverThread, "router/router receiver thread",
                Duration.ofSeconds(10));
            if (failure.get() != null) {
                throw new IllegalStateException("router/router receiver failed",
                    failure.get());
            }
            PerfUtil.printSingleMonitorAutoHwm(config, receiverMonitor, "receiver",
                SocketType.ROUTER);
            PerfUtil.printSingleMonitorAutoHwm(config, senderMonitor, "sender",
                SocketType.ROUTER);
            return metrics.finishSingle(config);
        } finally {
            if (!sharedContext) {
                senderCtx.close();
            }
            receiverCtx.close();
        }
    }

    // C parity: perf_router_router.cpp performs a PING/PONG handshake before
    // the active phase and uses the observed peer routing id for active and
    // stop-token sends. Without this step, websocket ROUTER->ROUTER can report
    // connection readiness before the explicit route is usable.
    static RoutingId performRouterRouterHandshake(RouterSocket receiver,
                                                  RouterSocket sender,
                                                  RoutingId receiverRoute,
                                                  RoutingId senderRouteFallback,
                                                  Duration timeout) {
        long deadline = System.nanoTime() + timeout.toNanos();
        RoutingId senderRoute = null;
        systems.zlink.contracts.messaging.Received received =
            new systems.zlink.contracts.messaging.Received();
        try {
            while (System.nanoTime() < deadline && senderRoute == null) {
                try (Message ping = Message.from(PING)) {
                    if (!trySendActive(sender, receiverRoute, ping)) {
                        Thread.onSpinWait();
                    }
                }
                while (recvIntoNoWait(receiver, received)) {
                    try {
                        if (Arrays.equals(received.firstPart().data(), PING)) {
                            senderRoute = received.getRoutingId().orElse(
                                senderRouteFallback);
                            break;
                        }
                    } finally {
                        received.close();
                    }
                }
                if (senderRoute == null) {
                    Thread.onSpinWait();
                }
            }
            if (senderRoute == null) {
                throw new IllegalStateException("router/router handshake ping timed out");
            }

            try (Message pong = Message.from(PONG)) {
                senderRouterSend(receiver, senderRoute, pong);
            }

            while (true) {
                try {
                    if (!sender.recv(received, RecvFlags.NONE)) {
                        continue;
                    }
                    try {
                        boolean ok = Arrays.equals(received.firstPart().data(), PONG);
                        if (!ok) {
                            throw new IllegalStateException(
                                "router/router handshake received unexpected payload");
                        }
                        return received.getRoutingId().orElse(receiverRoute);
                    } finally {
                        received.close();
                    }
                } catch (ZlinkRecvException ex) {
                    if (ex.getResult() == RecvResult.NO_DATA
                        || ex.getResult() == RecvResult.BUSY) {
                        continue;
                    }
                    throw ex;
                }
            }
        } finally {
            received.close();
        }
    }

    private static void drainRouterSocket(RouterSocket socket) {
        systems.zlink.contracts.messaging.Received received =
            new systems.zlink.contracts.messaging.Received();
        try {
            while (recvIntoNoWait(socket, received)) {
                received.close();
            }
        } finally {
            received.close();
        }
    }

    private static boolean recvIntoNoWait(RouterSocket socket,
                                          systems.zlink.contracts.messaging.Received received) {
        try {
            return socket.recv(received, RecvFlags.DONT_WAIT);
        } catch (ZlinkRecvException ex) {
            if (ex.getResult() == RecvResult.NO_DATA
                || ex.getResult() == RecvResult.BUSY) {
                return false;
            }
            throw ex;
        }
    }

    // C parity: perf_router_router.cpp send_router_stop_token sends the
    // terminator with ZLINK_SEND_FLAGS_NONE and retries transient failures.
    // DONT_WAIT can lose the only stop token while the route is settling.
    private static void sendRouterStopToken(RouterSocket sender,
                                            RoutingId targetRoute) {
        PerfStopToken.sendWithRetry(() -> {
            try (Message stop = PerfStopToken.newMessage()) {
                senderRouterSend(sender, targetRoute, stop);
                return true;
            }
        }, "router/router");
    }

    private static void senderRouterSend(RouterSocket socket,
                                         RoutingId route,
                                         Message message) {
        PerfUtil.awaitStage(socket.send(route)
            .message(message)
            .submit());
    }

    private static boolean trySendActive(RouterSocket sender, RoutingId route,
                                         Message active) {
        try {
            PerfUtil.awaitStage(sender.send(route)
                .message(active)
                .submit());
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

    private static boolean trySendBlocking(RouterSocket sender, RoutingId route,
                                           Message active) {
        try {
            PerfUtil.awaitStage(sender.send(route)
                .message(active)
                .submit());
            return true;
        } catch (systems.zlink.contracts.errors.ZlinkSubmitException ex) {
            if (ex.getResult()
                == systems.zlink.contracts.sockets.SubmitResult.BACKPRESSURED) {
                return false;
            }
            throw ex;
        } catch (systems.zlink.contracts.errors.ZlinkException ex) {
            int errno = ex.getNativeErrno();
            if (PerfErrno.isRetryableSend(errno)
                || errno == PerfErrno.ETIMEDOUT
                || errno == PerfErrno.EHOSTUNREACH
                || errno == PerfErrno.ENOTCONN) {
                return false;
            }
            throw ex;
        }
    }
}
