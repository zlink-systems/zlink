/* SPDX-License-Identifier: MPL-2.0 */

package systems.zlink.perf.multi;

import systems.zlink.contracts.core.Context;
import systems.zlink.contracts.sockets.DealerSocket;
import systems.zlink.contracts.messaging.Message;
import systems.zlink.contracts.eventing.MonitorEventType;
import systems.zlink.contracts.eventing.SocketMonitor;
import systems.zlink.contracts.eventing.PollEventFlags;
import systems.zlink.contracts.messaging.Received;
import systems.zlink.contracts.sockets.SocketType;
import systems.zlink.contracts.sockets.SendFlags;
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
import java.util.concurrent.CompletionStage;

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
            while (true) {
                long now = System.nanoTime();
                if (now >= measureDeadline) {
                    break;
                }
                pollSet.setEvents(0, PollEventFlags.POLLIN);
                // The configured active deadline owns phase termination even
                // when no traffic arrives. A wire stop wakes the poller but
                // does not replace this bounded measurement wait.
                int timeoutMs = remainingTimeoutMs(measureDeadline, now);
                int readyCount = pollSet.poll(timeoutMs);
                if (readyCount <= 0
                    || !pollSet.readyHasEventAt(0, PollEventFlags.POLLIN)) {
                    continue;
                }
                drainCounted(server, received, config, metrics,
                    measureDeadline);
                if (System.nanoTime() >= measureDeadline) {
                    break;
                }
            }
            // Tail drain after the measure deadline: receive any remaining
            // in-flight messages WITHOUT counting them so stale traffic does
            // not spill into the next size case (C drain_phase_until_idle,
            // count_message=false / collect_latency=false).
            drainTailUncounted(server, received, pollSet);
            received.close();
            return metrics.finishMulti(config);
        }
    }

    private static void drainCounted(DealerSocket server, Received received,
                                     PerfUtil.Config config,
                                     PerfUtil.Metrics metrics,
                                     long measureDeadline) {
        // C receives one message after a readiness wake, then checks the
        // deadline before draining further. This keeps the active boundary
        // from counting an already queued post-deadline tail.
        if (!PerfUtil.recvNoWait(server, received)) {
            return;
        }
        recordCounted(received, config, metrics);
        if (System.nanoTime() >= measureDeadline) {
            return;
        }
        while (true) {
            if (System.nanoTime() >= measureDeadline) {
                return;
            }
            if (!PerfUtil.recvNoWait(server, received)) {
                return;
            }
            recordCounted(received, config, metrics);
        }
    }

    private static void recordCounted(Received received,
                                      PerfUtil.Config config,
                                      PerfUtil.Metrics metrics) {
        if (received.parts().size() == 1
            && PerfStopToken.isStopTokenMessage(received.firstPart())) {
            return;
        }
        Message payload = PerfUtil.measurementPayload(received.parts());
        if (payload != null) {
            PerfUtil.recordActiveLatency(metrics, payload, config.size(), false);
        }
    }

    private static int remainingTimeoutMs(long deadline, long now) {
        long remainingNs = Math.max(1L, deadline - now);
        return (int) Math.min(Integer.MAX_VALUE,
            (remainingNs + 999_999L) / 1_000_000L);
    }

    private static void drainTailUncounted(DealerSocket server,
                                            Received received,
                                            PerfSocketPollSet pollSet) {
        // Bounded idle-based tail drain (C drain_phase_until_idle: 2s max,
        // 50ms idle window) so a quiet socket does not block teardown.
        long maxDeadline = System.nanoTime() + 2_000_000_000L;
        long idleDeadline = System.nanoTime() + 50_000_000L;
        while (System.nanoTime() < maxDeadline
            && System.nanoTime() < idleDeadline) {
            boolean progressed = false;
            while (true) {
                if (System.nanoTime() >= maxDeadline) {
                    return;
                }
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
                SocketMonitor monitor = client.monitorOpen(
                    config.monitorHwm(), MonitorEventType.CONNECTION_READY);
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
            PerfUtil.recalculateAutoHwm(ctx);
            for (int i = 0; i < monitors.size(); i++) {
                PerfUtil.printMultiMonitorAutoHwm(config, monitors.get(i),
                    "client", "client[" + i + "]", SocketType.DEALER);
            }
            PerfControl.emitClientReady(config.size());
            PerfControl.awaitStart(config.size(), "dealer/dealer client");
            long activeEnd = System.nanoTime()
                + config.durationSeconds() * 1_000_000_000L;
            PerfMultiRoutedSendCoordinator.runAdmissions(clients.size(),
                activeEnd,
                index -> sendOneActive(clients.get(index), config.size(),
                    activeEnd),
                Duration.ofSeconds(config.durationSeconds() + 5L),
                "multi dealer/dealer async sends");
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

    private static CompletionStage<Void> sendOneActive(DealerSocket socket,
                                                       int size,
                                                       long activeEnd) {
        try (Message payload = PerfUtil.payload(size,
                 (byte) PerfUtil.PHASE_ACTIVE, System.nanoTime());
             Message tail = PerfUtil.measurementPartCount() == 2
                 ? PerfUtil.measurementTail() : null) {
            if (PerfUtil.measurementPartCount() == 2) {
                return socket.send().message(payload)
                    .message(tail)
                    .timeout(PerfMultiAsyncSendLoop.remainingTimeout(activeEnd))
                    .submit();
            }
            return socket.send().message(payload)
                .timeout(PerfMultiAsyncSendLoop.remainingTimeout(activeEnd))
                .submit();
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
                socket.send().message(stop).submit_sync(SendFlags.NONE);
                return;
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

    private static boolean isTransient(ZlinkException ex) {
        if (ex instanceof ZlinkSubmitException submit) {
            return submit.getResult() == SubmitResult.BACKPRESSURED;
        }
        return ex.getNativeErrno() == 11 || ex.getNativeErrno() == 4;
    }
}
