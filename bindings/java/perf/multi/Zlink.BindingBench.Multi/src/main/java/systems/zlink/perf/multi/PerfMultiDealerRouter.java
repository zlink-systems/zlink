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
import systems.zlink.perf.PerfUtil;
import java.time.Duration;
import java.nio.charset.StandardCharsets;
import java.util.ArrayDeque;
import java.util.ArrayList;
import java.util.Deque;
import java.util.List;
import java.util.concurrent.atomic.AtomicBoolean;

final class PerfMultiDealerRouter {
    private static final MonitorEventType READY_EVENT =
        MonitorEventType.CONNECTION_READY;

    private PerfMultiDealerRouter() {
    }

    static PerfUtil.Result runServer(PerfUtil.Config config) {
        AtomicBoolean stopRequested = PerfControl.watchStopSignal("dealer-router server");
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
            Deque<PendingReply> pendingReplies = new ArrayDeque<>();
            systems.zlink.contracts.messaging.Received receivedBuffer = new systems.zlink.contracts.messaging.Received();
            try (PerfSocketPollSet pollSet = PerfSocketPollSet.fromSockets(
                List.of(server), PollEventFlags.POLLIN)) {
                // The C relay remains active until the runner sends STOP.
                // A client wire token is ordinary relay traffic, not a
                // server termination condition.
                while (!stopRequested.get()) {
                    if (pendingReplies.isEmpty()) {
                        pollSet.setEvents(0, PollEventFlags.POLLIN);
                    } else {
                        pollSet.setEvents(0, PollEventFlags.POLLIN,
                            PollEventFlags.POLLOUT);
                    }
                    int readyCount = pollSet.poll(50);
                    if (readyCount <= 0) {
                        continue;
                    }
                    boolean writable = pollSet.readyHasEventAt(0,
                        PollEventFlags.POLLOUT);
                    boolean readable = pollSet.readyHasEventAt(0,
                        PollEventFlags.POLLIN);
                    if (writable) {
                        flushPending(server, pendingReplies);
                    }
                    // Match the C relay: a DONT_WAIT drain starts only when
                    // the poller reported this socket readable.  Calling recv
                    // after every auxiliary STOP wake changes the hot path.
                    if (!readable) {
                        continue;
                    }
                    while (true) {
                        if (stopRequested.get()) {
                            break;
                        }
                        if (!server.recv(receivedBuffer, systems.zlink.contracts.sockets.RecvFlags.DONT_WAIT)) {
                            break;
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
                // The requester owns the active deadline. Bound the wait by
                // its remaining interval so a quiet relay cannot keep this
                // phase past the configured duration.
                int readyCount = pollSet.poll(remainingTimeoutMs(activeEnd));
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
            // C routed echo ends the relay through the runner control path.
            // Do not inject an extra routed stop frame into the measured
            // topology after the active client phase.
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

    private static int remainingTimeoutMs(long deadline) {
        long remainingNs = deadline - System.nanoTime();
        if (remainingNs <= 0) {
            return 0;
        }
        return (int) Math.min(Integer.MAX_VALUE,
            (remainingNs + 999_999L) / 1_000_000L);
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
