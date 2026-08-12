/* SPDX-License-Identifier: MPL-2.0 */

package systems.zlink.perf.multi;

import systems.zlink.contracts.core.Context;
import systems.zlink.contracts.sockets.DealerSocket;
import systems.zlink.contracts.messaging.Message;
import systems.zlink.contracts.eventing.MonitorEventType;
import systems.zlink.contracts.eventing.SocketMonitor;
import systems.zlink.contracts.eventing.PollEventFlags;
import systems.zlink.contracts.messaging.Received;
import systems.zlink.contracts.sockets.SendFlags;
import systems.zlink.contracts.sockets.Socket;
import systems.zlink.contracts.sockets.SocketType;
import systems.zlink.contracts.errors.ZlinkSubmitException;
import systems.zlink.contracts.sockets.SubmitResult;
import systems.zlink.contracts.errors.ZlinkException;
import systems.zlink.perf.PerfControl;
import systems.zlink.perf.PerfSocketPollSet;
import systems.zlink.perf.PerfStopToken;
import systems.zlink.perf.PerfUtil;
import java.util.ArrayList;
import java.time.Duration;
import java.util.List;

final class PerfMultiDealerDealer {
    private static final MonitorEventType READY_EVENT = MonitorEventType.CONNECTION_READY;

    private PerfMultiDealerDealer() {
    }

    static PerfUtil.Result runServer(PerfUtil.Config config) {
        try (Context ctx = PerfUtil.newContext(config);
             DealerSocket server = ctx.createDealerSocket();
             PerfSocketPollSet pollSet = PerfSocketPollSet.fromSockets(
                 List.of(server), PollEventFlags.POLLIN)) {
            PerfUtil.applySocketOptions(server, config);
            PerfUtil.configureServerTls(server, config.transport());
            server.bind(config.endpoint());
            PerfControl.emitReady(config.endpoint());
            PerfControl.awaitStart(config.size(), "dealer/dealer server");
            PerfUtil.recalculateAutoHwm(ctx);
            PerfUtil.printMultiSocketAutoHwm(config, server, "server",
                "server", SocketType.DEALER);
            PerfUtil.Metrics metrics = new PerfUtil.Metrics(config);
            Received received = new Received();
            // C parity: the active window advances only when socket readiness
            // wakes the poller. The stop token wakes a final drain but does
            // not determine the measured interval.
            long measureDeadline = System.nanoTime()
                + config.durationSeconds() * 1_000_000_000L;
            boolean stopSeen = false;
            while (true) {
                long now = System.nanoTime();
                if (now >= measureDeadline) {
                    break;
                }
                pollSet.setEvents(0, PollEventFlags.POLLIN);
                // A stop token can be consumed while a ready socket is
                // drained. Once that final wire signal is seen, wait only
                // for the remaining active interval. This prevents a later
                // empty -1 poll from keeping the server alive forever.
                int timeoutMs = stopSeen
                    ? remainingTimeoutMs(measureDeadline, now)
                    : -1;
                int readyCount = pollSet.poll(timeoutMs);
                if (readyCount <= 0
                    || !pollSet.readyHasEventAt(0, PollEventFlags.POLLIN)) {
                    continue;
                }
                stopSeen |= drainCounted(server, received, config, metrics,
                    measureDeadline);
                if (System.nanoTime() >= measureDeadline) {
                    break;
                }
            }
            // Tail drain after the measure deadline: receive any remaining
            // in-flight messages WITHOUT counting them so stale traffic does
            // not spill into the next size case (C drain_phase_until_idle,
            // count_message=false / collect_latency=false).
            drainTailUncounted(server, received, config, pollSet);
            received.close();
            return metrics.finishMulti(config);
        }
    }

    private static boolean drainCounted(DealerSocket server, Received received,
                                        PerfUtil.Config config,
                                        PerfUtil.Metrics metrics,
                                        long measureDeadline) {
        // C receives one message after a readiness wake, then checks the
        // deadline before draining further. This keeps the active boundary
        // from counting an already queued post-deadline tail.
        if (!PerfUtil.recvNoWait(server, received)) {
            return false;
        }
        boolean stopSeen = recordCounted(received, config, metrics);
        if (System.nanoTime() >= measureDeadline) {
            return stopSeen;
        }
        while (true) {
            if (!PerfUtil.recvNoWait(server, received)) {
                return stopSeen;
            }
            stopSeen |= recordCounted(received, config, metrics);
        }
    }

    private static boolean recordCounted(Received received,
                                         PerfUtil.Config config,
                                         PerfUtil.Metrics metrics) {
        if (PerfStopToken.isStopTokenMessage(received.firstPart())) {
            return true;
        }
        PerfUtil.recordActiveLatency(metrics, received.firstPart(),
            config.size(), false);
        return false;
    }

    private static int remainingTimeoutMs(long deadline, long now) {
        long remainingNs = Math.max(1L, deadline - now);
        return (int) Math.min(Integer.MAX_VALUE,
            (remainingNs + 999_999L) / 1_000_000L);
    }

    private static void drainTailUncounted(DealerSocket server,
                                            Received received,
                                            PerfUtil.Config config,
                                            PerfSocketPollSet pollSet) {
        // Bounded idle-based tail drain (C drain_phase_until_idle: 2s max,
        // 50ms idle window) so a quiet socket does not block teardown.
        long maxDeadline = System.nanoTime() + 2_000_000_000L;
        long idleDeadline = System.nanoTime() + 50_000_000L;
        while (System.nanoTime() < maxDeadline
            && System.nanoTime() < idleDeadline) {
            boolean progressed = false;
            while (true) {
                if (!PerfUtil.recvNoWait(server, received)) {
                    break;
                }
                progressed = true;
            }
            if (progressed) {
                idleDeadline = System.nanoTime() + 50_000_000L;
                continue;
            }
            pollSet.setEvents(0, PollEventFlags.POLLIN);
            pollSet.poll(50);
        }
    }

    static PerfUtil.Result runClient(PerfUtil.Config config) {
        List<SocketMonitor> monitors = new ArrayList<>(config.clients());
        List<DealerSocket> clients = new ArrayList<>(config.clients());
        Context ctx = PerfUtil.newContext(config);
        try {
            for (int i = 0; i < config.clients(); i++) {
                DealerSocket client = ctx.createDealerSocket();
                SocketMonitor monitor = client.monitorOpen(MonitorEventType.CONNECTION_READY);
                PerfUtil.applyMonitorOptions(monitor, config);
                PerfUtil.applySocketOptions(client, config);
                PerfUtil.configureClientTls(client, config.transport());
                client.connect(config.endpoint());
                clients.add(client);
                monitors.add(monitor);
            }
            Duration readyTimeout = Duration.ofMillis(config.connectReadyTimeoutMs());
            for (int i = 0; i < monitors.size(); i++) {
                PerfUtil.waitForMonitorEvent(monitors.get(i), READY_EVENT, 1,
                    readyTimeout, "dealer/dealer client ready");
            }
            for (DealerSocket client : clients) {
            }
            PerfUtil.recalculateAutoHwm(ctx);
            for (int i = 0; i < monitors.size(); i++) {
                PerfUtil.printMultiMonitorAutoHwm(config, monitors.get(i),
                    "client", "client[" + i + "]", SocketType.DEALER);
            }
            PerfControl.emitClientReady(config.size());
            PerfControl.awaitStart(config.size(), "dealer/dealer client");
            List<systems.zlink.contracts.sockets.Socket> pollSockets = new ArrayList<>(clients.size());
            pollSockets.addAll(clients);
            try (PerfSocketPollSet pollSet = PerfSocketPollSet.fromSockets(
                     pollSockets, PollEventFlags.POLLOUT)) {
                for (int i = 0; i < clients.size(); i++) {
                    pollSet.setEvents(i);
                }
                boolean[] pending = new boolean[clients.size()];
                long activeEnd = System.nanoTime() + config.durationSeconds() * 1_000_000_000L;
                while (System.nanoTime() < activeEnd) {
                    boolean progressed = false;
                    boolean hasPending = false;
                    for (int index = 0; index < clients.size(); index++) {
                        if (pending[index]) {
                            hasPending = true;
                            continue;
                        }
                        // C parity: perf_multi_dealer_dealer_client.cpp
                        // run_send_window (~190-214) sends on a socket in a
                        // tight inner loop UNTIL it backpressures, then marks
                        // it pending and moves on. The prior one-message-per-
                        // socket round-robin meant a freshly stamped message
                        // waited for ~99 other sockets' send work before its
                        // io-thread flush, inflating one-way send-queue
                        // residence (latency) on tls/wss. Draining each
                        // socket to its HWM in a burst (re-stamping every
                        // message) keeps stamp-to-wire tight like C.
                        DealerSocket socket = clients.get(index);
                        while (System.nanoTime() < activeEnd) {
                            if (sendOneActive(socket, config.size(),
                                    activeEnd)) {
                                progressed = true;
                                continue;
                            }
                            pending[index] = true;
                            pollSet.setEvents(index, PollEventFlags.POLLOUT);
                            hasPending = true;
                            break;
                        }
                    }
                    if (progressed || !hasPending) {
                        continue;
                    }
                    // PERF_MULTI_TEST_POLICY § 1.3.1: signal-driven wait for
                    // POLLOUT readiness; no timer-based fallback.
                    pollWritable(pollSet, pending, -1);
                }
            }
            // C parity: send one wire-level stop token on every client socket
            // so the server's signal-driven receive loop is guaranteed to wake.
            for (DealerSocket client : clients) {
                sendStopTokenBlocking(client);
            }
            PerfControl.emitClientDone(config.size());
            return PerfUtil.Result.silent(config);
        } finally {
            for (SocketMonitor monitor : monitors) {
                try {
                    monitor.close();
                } catch (Exception e) {
                    System.err.println("[multi-dealer-dealer] cleanup failed: " + e);
                }
            }
            for (DealerSocket client : clients) {
                try {
                    client.disconnect(config.endpoint());
                } catch (Exception e) {
                    System.err.println("[multi-dealer-dealer] cleanup failed: " + e);
                }
                try {
                    client.close();
                } catch (Exception e) {
                    System.err.println("[multi-dealer-dealer] cleanup failed: " + e);
                }
            }
            try {
                ctx.close();
            } catch (Exception e) {
                System.err.println("[multi-dealer-dealer] cleanup failed: " + e);
            }
        }
    }

    // C parity: perf_multi_dealer_dealer_client.cpp send_one_message. Stamp a
    // fresh payload immediately before the non-blocking send. Returns true on
    // send_status_ok, false on transient backpressure (send_status_blocked);
    // a non-transient error is fatal.
    private static boolean sendOneActive(DealerSocket socket,
                                         int size, long activeEnd) {
        Message payload = PerfUtil.payload(size, (byte) PerfUtil.PHASE_ACTIVE,
            System.nanoTime());
        try (payload) {
            return socket.send().message(payload)
                .flags(SendFlags.DONT_WAIT).submit();
        } catch (ZlinkSubmitException ex) {
            if (isTransient(ex)) {
                return false;
            }
            throw ex;
        } catch (ZlinkException ex) {
            if (isTransient(ex)) {
                return false;
            }
            throw ex;
        }
    }

    private static void sendStopTokenBlocking(DealerSocket socket) {
        // C send_stop_token uses ZLINK_SEND_FLAGS_NONE, not a DONT_WAIT
        // retry loop. The socket send timeout bounds each retry under the
        // benchmark policy. Waiting forever for POLLOUT after the server's
        // measurement deadline can otherwise keep this process alive after
        // the peer stopped draining its queue.
        while (true) {
            try (Message stop = PerfStopToken.newMessage()) {
                if (socket.send().message(stop).flags(SendFlags.NONE).submit()) {
                    return;
                }
            } catch (ZlinkSubmitException ex) {
                if (!isTransient(ex)) {
                    throw ex;
                }
            } catch (ZlinkException ex) {
                if (!isTransient(ex)) {
                    throw ex;
                }
            }
        }
    }

    private static void pollWritable(PerfSocketPollSet pollSet, boolean[] pending,
                                     int timeoutMs) {
        int readyCount = pollSet.poll(timeoutMs);
        if (readyCount <= 0) {
            return;
        }
        for (int readyOffset = 0; readyOffset < readyCount; readyOffset++) {
            int i = pollSet.readyIndexAt(readyOffset);
            if (!pending[i]) {
                continue;
            }
            if (!pollSet.readyHasEventAt(readyOffset, PollEventFlags.POLLOUT)) {
                continue;
            }
            pending[i] = false;
            pollSet.setEvents(i);
        }
    }

    private static boolean isTransient(ZlinkException ex) {
        if (ex instanceof ZlinkSubmitException submit) {
            return submit.getResult() == SubmitResult.BACKPRESSURED;
        }
        return ex.getNativeErrno() == 11 || ex.getNativeErrno() == 4;
    }
}
