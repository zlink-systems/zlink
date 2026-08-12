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
import systems.zlink.perf.PerfMessageTemplatePool;
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
            // C parity (perf_multi_dealer_dealer_server.cpp run_receive_window
            // ~240-301): the counted throughput window is closed by a
            // server-side measure-seconds deadline, not by the stop token. The
            // signal-driven (-1) poll only wakes the loop; the stop token wakes
            // the poll too but does not anchor the aggregation end.
            //
            // The deadline MUST be re-checked *after* draining a poll wakeup
            // (matching C's post-receive / post-drain `if (now >= deadline)
            // break;` at lines 273-275 and 294-296). The client sends an
            // active window of identical duration and then exits. The server
            // and client start their windows at slightly different wall-clock
            // instants (the server clock starts only after its post-START
            // auto-HWM recalc), so the client can finish its active phase,
            // emit its stop token(s), and exit while the server still believes
            // it is a hair before its own deadline. If the server then looped
            // back into an unbounded perf_socket_poll(-1) there would be no
            // remaining peer to ever wake it, hanging until the harness
            // timeout. C tolerates the unconditional -1 only because its
            // deadline break is evaluated right after the wakeup is consumed;
            // to keep that termination guarantee robust against the cross-
            // process start-clock skew the blocking poll is bounded by the
            // time remaining to the deadline. Data and the wire stop token
            // still wake the poll immediately (signal-driven, no busy poll);
            // the bound only ensures the measure window cannot outlive its
            // deadline when the peer has already gone.
            long measureDeadline = System.nanoTime()
                + config.durationSeconds() * 1_000_000_000L;
            while (true) {
                long remainingNanos = measureDeadline - System.nanoTime();
                if (remainingNanos <= 0) {
                    break;
                }
                long remainingMs = Math.max(1L,
                    remainingNanos / 1_000_000L);
                pollSet.setEvents(0, PollEventFlags.POLLIN);
                int readyCount = pollSet.poll((int) Math.min(remainingMs,
                    (long) Integer.MAX_VALUE));
                if (readyCount <= 0
                    || !pollSet.readyHasEventAt(0, PollEventFlags.POLLIN)) {
                    continue;
                }
                drainCounted(server, received, config, metrics);
                // Post-drain deadline check (C lines 273-275 / 294-296):
                // break here so the iteration that consumed the final
                // traffic + stop token terminates instead of re-entering the
                // poll after the peer has gone.
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

    private static void drainCounted(DealerSocket server, Received received,
                                      PerfUtil.Config config,
                                      PerfUtil.Metrics metrics) {
        while (true) {
            if (!PerfUtil.recvNoWait(server, received)) {
                return;
            }
            if (PerfStopToken.isStopTokenMessage(received.firstPart())) {
                // Stop token only wakes the poll; it never ends the
                // counted window (matches C, which ignores stop_count).
                continue;
            }
            PerfUtil.recordActiveLatency(metrics, received.firstPart(),
                config.size(), false);
        }
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
                try (PerfMessageTemplatePool payloadPool =
                         new PerfMessageTemplatePool(config.size(), 256)) {
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
                            if (sendOneActive(socket, payloadPool, config.size(),
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
                                         PerfMessageTemplatePool payloadPool,
                                         int size, long activeEnd) {
        Message payload = payloadPool.acquire(size,
            (byte) PerfUtil.PHASE_ACTIVE, System.nanoTime(), activeEnd);
        if (payload == null) {
            return false;
        }
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
