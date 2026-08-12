/* SPDX-License-Identifier: MPL-2.0 */

package systems.zlink.perf.multi;

import systems.zlink.contracts.core.Context;
import systems.zlink.contracts.sockets.DealerSocket;
import systems.zlink.contracts.messaging.Message;
import systems.zlink.contracts.eventing.MonitorEventType;
import systems.zlink.contracts.eventing.SocketMonitor;
import systems.zlink.contracts.eventing.PollEventFlags;
import systems.zlink.contracts.messaging.Received;
import systems.zlink.contracts.sockets.RecvFlags;
import systems.zlink.contracts.sockets.RouterSocket;
import systems.zlink.contracts.core.RoutingId;
import systems.zlink.contracts.sockets.SendFlags;
import systems.zlink.contracts.sockets.Socket;
import systems.zlink.contracts.sockets.SocketType;
import systems.zlink.contracts.errors.ZlinkException;
import systems.zlink.contracts.core.RoutingId;
import systems.zlink.perf.PerfControl;
import systems.zlink.perf.PerfSocketPollSet;
import systems.zlink.perf.PerfStopToken;
import systems.zlink.perf.PerfUtil;
import java.time.Duration;
import java.nio.charset.StandardCharsets;
import java.util.ArrayDeque;
import java.util.ArrayList;
import java.util.Deque;
import java.util.List;

final class PerfMultiDealerRouter {
    private static final MonitorEventType READY_EVENT =
        MonitorEventType.CONNECTION_READY;

    private PerfMultiDealerRouter() {
    }

    static PerfUtil.Result runServer(PerfUtil.Config config) {
        try (Context ctx = PerfUtil.newContext(config);
             RouterSocket server = ctx.createRouterSocket();
             var monitor = server.monitorOpen(MonitorEventType.CONNECTION_READY)) {
            PerfUtil.applyMonitorOptions(monitor, config);
            PerfUtil.applySocketOptions(server, config);
            PerfUtil.configureServerTls(server, config.transport());
            server.bind(config.endpoint());
            PerfControl.emitReady(config.endpoint());
            // The receive loop is the readiness barrier for routed clients.
            // On Windows, a monitor event can be consumed before the
            // aggregate count reaches the expected client count even though
            // the corresponding socket is usable. Waiting for that count
            // makes the case fail before it can measure traffic; the poller
            // below admits each connection and the stop-token count still
            // requires every client to complete its phase.
            PerfUtil.recalculateAutoHwm(ctx);
            PerfUtil.printMultiMonitorAutoHwm(config, monitor, "server",
                "server", SocketType.ROUTER);
            int stops = 0;
            Deque<PendingReply> pendingReplies = new ArrayDeque<>();
            systems.zlink.contracts.messaging.Received receivedBuffer = new systems.zlink.contracts.messaging.Received();
            try (PerfSocketPollSet pollSet = PerfSocketPollSet.fromSockets(
                List.of(server), PollEventFlags.POLLIN)) {
                // PERF_MULTI_TEST_POLICY § 1.3.1: signal-driven wait (-1);
                // server exits after observing one wire-level stop token per
                // expected client.
                while (stops < config.clients()) {
                    if (pendingReplies.isEmpty()) {
                        pollSet.setEvents(0, PollEventFlags.POLLIN);
                    } else {
                        pollSet.setEvents(0, PollEventFlags.POLLIN,
                            PollEventFlags.POLLOUT);
                    }
                    int readyCount = pollSet.poll(-1);
                    if (readyCount > 0
                        && pollSet.readyHasEventAt(0, PollEventFlags.POLLOUT)) {
                        flushPending(server, pendingReplies);
                    }
                    while (true) {
                        if (!server.recv(receivedBuffer, systems.zlink.contracts.sockets.RecvFlags.DONT_WAIT)) {
                            break;
                        }
                        if (PerfStopToken.isStopTokenMessage(receivedBuffer.firstPart())) {
                            stops++;
                            receivedBuffer.close();
                            continue;
                        }
                        if (pendingReplies.isEmpty()
                            && receivedBuffer.send()
                                .message(receivedBuffer.firstPart())
                                .flags(SendFlags.DONT_WAIT)
                                .submit()) {
                            continue;
                        }
                        RoutingId rid = receivedBuffer.getRoutingId().orElseThrow();
                        Message reply = Message.from(receivedBuffer.firstPart());
                        receivedBuffer.close();
                        if (pendingReplies.isEmpty()
                            && server.send(rid)
                                .message(reply)
                                .flags(SendFlags.DONT_WAIT)
                                .submit()) {
                            reply.close();
                        } else {
                            pendingReplies.addLast(new PendingReply(rid, reply));
                        }
                    }
                }
            }
            receivedBuffer.close();
            return PerfUtil.Result.silent(config);
        }
    }

    static PerfUtil.Result runClient(PerfUtil.Config config) {
        PerfUtil.Metrics metrics = new PerfUtil.Metrics(config);
        List<SocketMonitor> monitors = new ArrayList<>(config.clients());
        List<DealerSocket> clients = new ArrayList<>(config.clients());
        Context ctx = PerfUtil.newContext(config);
        try {
            for (int i = 0; i < config.clients(); i++) {
                DealerSocket client = ctx.createDealerSocket();
                client.setRoutingId(RoutingId.from(
                    ("PERF_DEALER_" + i).getBytes(StandardCharsets.UTF_8)));
                SocketMonitor monitor = client.monitorOpen(MonitorEventType.CONNECTION_READY);
                PerfUtil.applyMonitorOptions(monitor, config);
                PerfUtil.applySocketOptions(client, config);
                PerfUtil.configureClientTls(client, config.transport());
                client.connect(config.endpoint());
                clients.add(client);
                monitors.add(monitor);
            }
            Duration readyTimeout = Duration.ofMillis(
                config.connectReadyTimeoutMs());
            for (SocketMonitor monitor : monitors) {
                PerfUtil.waitForMonitorEvent(monitor, READY_EVENT, 1,
                    readyTimeout, "dealer/router client ready");
            }
            PerfUtil.recalculateAutoHwm(ctx);
            // C parity: the C dealer/router perf client emits its client-side
            // AUTO_HWM_DETAIL (component=client, dealer socket) so the report's
            // "Auto-HWM detail:" table carries both the client AND server rows.
            // Emit BEFORE closing the monitors -- matching PerfMultiRouterRouter
            // ordering. The previous code closed+cleared `monitors` first, so
            // the print loop ran over an empty list and the client row was
            // silently dropped (only the server row reached the report).
            for (int i = 0; i < monitors.size(); i++) {
                PerfUtil.printMultiMonitorAutoHwm(config, monitors.get(i),
                    "client", "client[" + i + "]", SocketType.DEALER);
            }
            for (int i = 0; i < monitors.size(); i++) {
                monitors.get(i).close();
            }
            monitors.clear();
            runDealerRouterClientLoop(clients, config, metrics);
            return metrics.finishMulti(config);
        } finally {
            for (SocketMonitor monitor : monitors) {
                try {
                    monitor.close();
                } catch (Exception ignored) {
                }
            }
            for (DealerSocket client : clients) {
                try {
                    client.close();
                } catch (Exception ignored) {
                }
            }
            try {
                ctx.close();
            } catch (Exception ignored) {
            }
        }
    }

    private static void runDealerRouterClientLoop(List<DealerSocket> clients,
                                                  PerfUtil.Config config,
                                                  PerfUtil.Metrics metrics) {
        int n = clients.size();
        int msgSize = config.size();
        Message[] payloads = new Message[n];
        boolean[] waitingReply = new boolean[n];
        boolean[] waitingWritable = new boolean[n];
        for (int i = 0; i < n; i++) {
            payloads[i] = PerfUtil.payloadTemplate(msgSize);
        }
        List<systems.zlink.contracts.sockets.Socket> socketsAsBase = new ArrayList<>(n);
        socketsAsBase.addAll(clients);
        int rrIndex = 0;
        systems.zlink.contracts.messaging.Received replyBuffer = new systems.zlink.contracts.messaging.Received();
        try (PerfSocketPollSet pollSet = PerfSocketPollSet.fromSockets(
                socketsAsBase, PollEventFlags.POLLIN)) {
            long activeEnd = System.nanoTime()
                + (long) config.durationSeconds() * 1_000_000_000L;
            while (System.nanoTime() < activeEnd) {
                int startIndex = rrIndex;
                for (int i = 0; i < n; i++) {
                    int idx = (startIndex + i) % n;
                    if (waitingReply[idx] || waitingWritable[idx]) continue;
                    payloads[idx] = PerfUtil.resetAndWritePayload(payloads[idx], msgSize,
                        (byte) PerfUtil.PHASE_ACTIVE, System.nanoTime());
                    if (trySendPayload(clients.get(idx), payloads[idx])) {
                        waitingReply[idx] = true;
                    } else {
                        waitingWritable[idx] = true;
                    }
                    updatePollMask(pollSet, idx, waitingReply[idx],
                        waitingWritable[idx]);
                }
                rrIndex = (startIndex + 1) % n;
                // Match C run_echo_window_round_robin: traffic readiness, not
                // an arbitrary timer tick, advances the active loop. The
                // deadline is checked at the next loop boundary after this
                // ready set is drained.
                int readyCount = pollSet.poll(-1);
                for (int readyOffset = 0; readyOffset < readyCount; readyOffset++) {
                    int idx = pollSet.readyIndexAt(readyOffset);
                    boolean writable =
                        pollSet.readyHasEventAt(readyOffset,
                            PollEventFlags.POLLOUT);
                    if (writable && waitingWritable[idx] && !waitingReply[idx]) {
                        if (trySendPayload(clients.get(idx), payloads[idx])) {
                            waitingWritable[idx] = false;
                            waitingReply[idx] = true;
                            updatePollMask(pollSet, idx, true, false);
                        }
                    }
                    boolean readable =
                        pollSet.readyHasEventAt(readyOffset,
                            PollEventFlags.POLLIN);
                    if (!readable) continue;
                    drainReplies(clients.get(idx), idx, waitingReply,
                        waitingWritable, msgSize, metrics, pollSet,
                        replyBuffer);
                }
            }
            replyBuffer.close();
            Message.closeAll(List.of(payloads));
            for (DealerSocket client : clients) {
                try (Message stop = PerfStopToken.newMessage();
                     PerfSocketPollSet stopPoll = PerfSocketPollSet.fromSockets(
                         List.of(client), PollEventFlags.POLLOUT)) {
                    stopPoll.setEvents(0);
                    sendUntilSent(client, stopPoll, stop,
                        System.nanoTime() + Duration.ofSeconds(5).toNanos());
                }
            }
        }
    }

    private static boolean trySendPayload(DealerSocket client, Message payload) {
        return client.send().message(payload).flags(SendFlags.DONT_WAIT).submit();
    }

    private static void drainReplies(DealerSocket client, int idx,
                                     boolean[] waitingReply,
                                     boolean[] waitingWritable,
                                     int msgSize,
                                     PerfUtil.Metrics metrics,
                                     PerfSocketPollSet pollSet,
                                     systems.zlink.contracts.messaging.Received replyBuffer) {
        while (true) {
            if (!client.recv(replyBuffer, systems.zlink.contracts.sockets.RecvFlags.DONT_WAIT)) {
                break;
            }
            if (!waitingReply[idx]) {
                replyBuffer.close();
                continue;
            }
            PerfUtil.recordActiveLatency(metrics, replyBuffer.firstPart(),
                msgSize, true);
            waitingReply[idx] = false;
            waitingWritable[idx] = false;
            updatePollMask(pollSet, idx, false, false);
        }
    }

    private static void updatePollMask(PerfSocketPollSet pollSet, int idx,
                                       boolean waitingReply,
                                       boolean waitingWritable) {
        if (waitingWritable && !waitingReply) {
            pollSet.setEvents(idx, PollEventFlags.POLLIN,
                PollEventFlags.POLLOUT);
        } else {
            pollSet.setEvents(idx, PollEventFlags.POLLIN);
        }
    }

    private static void sendUntilSent(DealerSocket client, PerfSocketPollSet pollSet,
                                      Message part,
                                      long deadlineNs) {
        while (true) {
            if (client.send().message(part).flags(SendFlags.DONT_WAIT).submit()) {
                return;
            }
            if (System.nanoTime() >= deadlineNs) {
                throw new IllegalStateException("dealer/router send timed out");
            }
            // PERF_MULTI_TEST_POLICY § 1.3.1: wait for POLLOUT readiness.
            // The finite wait preserves the application-level deadline when
            // the peer has already stopped producing a readiness transition.
            long remainingNs = deadlineNs - System.nanoTime();
            int waitMs = (int) Math.min(Integer.MAX_VALUE,
                Math.max(1L, remainingNs / 1_000_000L));
            pollSet.setEvents(0, PollEventFlags.POLLOUT);
            pollSet.poll(waitMs);
        }
    }

    private static boolean awaitReadable(PerfSocketPollSet pollSet, long deadlineNs) {
        // PERF_MULTI_TEST_POLICY § 1.3.1: signal-driven wait for POLLIN.
        // Deadline check is application-level; poller timeout is -1.
        while (System.nanoTime() < deadlineNs) {
            try {
                pollSet.setEvents(0, PollEventFlags.POLLIN);
                if (pollSet.poll(-1) > 0) {
                    return true;
                }
            } catch (systems.zlink.contracts.errors.ZlinkException ex) {
                if (ex.getNativeErrno() != 11 && ex.getNativeErrno() != 4) {
                    throw ex;
                }
            }
        }
        return false;
    }

    private static void flushPending(RouterSocket server,
                                     Deque<PendingReply> pendingReplies) {
        while (!pendingReplies.isEmpty()) {
            PendingReply pending = pendingReplies.peekFirst();
            if (pending == null) {
                return;
            }
            if (!server.send(pending.routingId())
                .message(pending.payload())
                .flags(SendFlags.DONT_WAIT)
                .submit()) {
                return;
            }
            pendingReplies.removeFirst();
            pending.close();
        }
    }

    private record PendingReply(RoutingId routingId, Message payload) {
        private void close() {
            payload.close();
        }
    }
}
