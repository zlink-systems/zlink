/* SPDX-License-Identifier: MPL-2.0 */

package systems.zlink.perf.multi;

import systems.zlink.contracts.core.Context;
import systems.zlink.contracts.messaging.Message;
import systems.zlink.contracts.eventing.MonitorEventType;
import systems.zlink.contracts.eventing.SocketMonitor;
import systems.zlink.contracts.eventing.PollEventFlags;
import systems.zlink.contracts.messaging.Received;
import systems.zlink.contracts.errors.ZlinkRecvException;
import systems.zlink.contracts.sockets.RecvFlags;
import systems.zlink.contracts.sockets.RecvResult;
import systems.zlink.contracts.sockets.RouterSocket;
import systems.zlink.contracts.core.RoutingId;
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
import java.nio.charset.StandardCharsets;
import java.time.Duration;
import java.util.ArrayDeque;
import java.util.ArrayList;
import java.util.Deque;
import java.util.List;

final class PerfMultiRouterRouter {
    private static final MonitorEventType READY_EVENT =
        MonitorEventType.CONNECTION_READY;
    private static final RoutingId SERVER_ID = RoutingId.from(
        "PERF_SERVER".getBytes(StandardCharsets.UTF_8));

    private PerfMultiRouterRouter() {
    }

    static PerfUtil.Result runServer(PerfUtil.Config config) {
        try (Context ctx = PerfUtil.newContext(config);
             RouterSocket server = ctx.createRouterSocket();
             var monitor = server.monitorOpen(MonitorEventType.CONNECTION_READY)) {
            server.setRoutingId(SERVER_ID);
            PerfUtil.applyMonitorOptions(monitor, config);
            PerfUtil.applySocketOptions(server, config);
            PerfUtil.configureServerTls(server, config.transport());
            server.bind(config.endpoint());
            PerfControl.emitReady(config.endpoint());
            PerfUtil.waitForMonitorEvent(monitor, READY_EVENT, config.clients(),
                Duration.ofMillis(config.connectReadyTimeoutMs()),
                "router/router server ready");
            PerfUtil.recalculateAutoHwm(ctx);
            PerfUtil.printMultiMonitorAutoHwm(config, monitor, "server",
                "server", SocketType.ROUTER);
            Deque<PendingReply> pendingReplies = new ArrayDeque<>();
            // Long-lived caller-provided Received reused across every recv on
            // the server hot path. The binding refills its internal state in
            // place via adoptFrom, avoiding the per-recv Received allocation
            // that the legacy `recv() -> Received` path forced. Matches the
            // C++/.NET canonical caller-provided storage pattern.
            systems.zlink.contracts.messaging.Received receivedBuffer = new systems.zlink.contracts.messaging.Received();
            try (PerfSocketPollSet pollSet = PerfSocketPollSet.fromSockets(
                List.of(server), PollEventFlags.POLLIN)) {
                int stops = 0;
                while (stops < config.clients()) {
                    if (pendingReplies.isEmpty()) {
                        pollSet.setEvents(0, PollEventFlags.POLLIN);
                    } else {
                        pollSet.setEvents(0,
                            PollEventFlags.POLLIN, PollEventFlags.POLLOUT);
                    }
                    int readyCount = pollSet.poll(-1);
                    boolean writable = readyCount > 0
                        && pollSet.readyHasEventAt(0, PollEventFlags.POLLOUT);
                    boolean readable = readyCount > 0
                        && pollSet.readyHasEventAt(0, PollEventFlags.POLLIN);
                    if (writable) {
                        flushPending(server, pendingReplies);
                    }
                    if (!readable) {
                        continue;
                    }
                    while (true) {
                        boolean ok;
                        try {
                            ok = server.recv(receivedBuffer, RecvFlags.DONT_WAIT);
                        } catch (ZlinkRecvException ex) {
                            if (ex.getResult() == RecvResult.NO_DATA
                                || ex.getResult() == RecvResult.BUSY) {
                                break;
                            }
                            throw ex;
                        }
                        if (!ok) break;

                        if (PerfStopToken.isStopTokenMessage(
                                receivedBuffer.firstPart())) {
                            stops++;
                            receivedBuffer.close();
                            continue;
                        }
                        // Fast path: send directly when no pending backlog.
                        if (pendingReplies.isEmpty()
                            && receivedBuffer.send()
                                .message(receivedBuffer.firstPart())
                                .flags(SendFlags.DONT_WAIT)
                                .submit()) {
                            continue;
                        }
                        RoutingId rid = receivedBuffer.getRoutingId().orElseThrow();
                        // Slow path: take ownership of the part to outlive
                        // the Received scope and enqueue / send.
                        Message ownedReply = Message.from(receivedBuffer.firstPart());
                        receivedBuffer.close();
                        if (pendingReplies.isEmpty()
                            && server.send(rid)
                                .message(ownedReply)
                                .flags(SendFlags.DONT_WAIT)
                                .submit()) {
                            ownedReply.close();
                        } else {
                            pendingReplies.addLast(
                                new PendingReply(rid, ownedReply));
                        }
                    }
                }
            }
            receivedBuffer.close();
            return PerfUtil.Result.silent(config);
        }
    }

    // PERF_MULTI_TEST_POLICY §1.3 mandates a single app thread driving the
    // poller event loop for all N client sockets, mirroring the C reference
    // perf_multi_client_helpers.hpp::run_echo_window_round_robin. This
    // single-thread + single-context + N-sockets model is the canonical
    // multi-client measurement structure; the previous per-thread + per-
    // context fan-out diverged from policy and inflated measurement noise
    // (per-context I/O dispatcher overhead, synchronized metric collection).
    static PerfUtil.Result runClient(PerfUtil.Config config) {
        PerfUtil.Metrics metrics = new PerfUtil.Metrics(config);
        int clientCount = Math.max(1, config.clients());
        int durationSeconds = Math.max(1, config.durationSeconds());

        try (Context ctx = PerfUtil.newContext(config)) {
            List<RouterSocket> clients = new ArrayList<>(clientCount);
            List<systems.zlink.contracts.eventing.SocketMonitor> monitors =
                new ArrayList<>(clientCount);
            try {
                for (int i = 0; i < clientCount; i++) {
                    RouterSocket client = ctx.createRouterSocket();
                    client.setRoutingId(RoutingId.from(
                        ("PERF_CLIENT_" + i).getBytes(StandardCharsets.UTF_8)));
                    client.options().setConnectRoutingId(SERVER_ID);
                    var monitor = client.monitorOpen(
                        MonitorEventType.CONNECTION_READY);
                    PerfUtil.applyMonitorOptions(monitor, config);
                    PerfUtil.applySocketOptions(client, config);
                    PerfUtil.configureClientTls(client, config.transport());
                    client.connect(config.endpoint());
                    clients.add(client);
                    monitors.add(monitor);
                }
                for (int i = 0; i < clientCount; i++) {
                    PerfUtil.waitForMonitorEvent(monitors.get(i), READY_EVENT, 1,
                        Duration.ofMillis(config.connectReadyTimeoutMs()),
                        "router/router client ready[" + i + "]");
                }
                for (int i = 0; i < clientCount; i++) {
                }
                PerfUtil.recalculateAutoHwm(ctx);
                for (int i = 0; i < clientCount; i++) {
                    PerfUtil.printMultiMonitorAutoHwm(config, monitors.get(i),
                        "client", "client[" + i + "]", SocketType.ROUTER);
                }
                for (int i = 0; i < clientCount; i++) {
                    monitors.get(i).close();
                }
                monitors.clear();

                runRouterRouterClientLoop(ctx, clients, config, durationSeconds,
                    metrics);
            } finally {
                for (var monitor : monitors) {
                    try { monitor.close(); } catch (RuntimeException ignored) {}
                }
                for (RouterSocket client : clients) {
                    try { client.close(); } catch (RuntimeException ignored) {}
                }
            }
        }
        return metrics.finishMulti(config);
    }

    // Single-thread round-robin: one PollSet over all client sockets, mirror
    // of C perf_multi_client_helpers.hpp::run_echo_window_round_robin and
    // .NET PerfMultiRouterRouterClient.RunMultiRouterRouterClientLoop.
    private static void runRouterRouterClientLoop(Context ctx,
                                                  List<RouterSocket> clients,
                                                  PerfUtil.Config config,
                                                  int durationSeconds,
                                                  PerfUtil.Metrics metrics) {
        int n = clients.size();
        int msgSize = config.size();
        // Reusable per-slot send payload buffer. C reference reuses one
        // payload buffer across the entire active phase (or per-socket when
        // borrow_payload_per_socket=true). We use per-socket buffers so that
        // an inflight=1 send isn't disturbed by the next slot's stamp.
        Message[] payloads = new Message[n];
        boolean[] waitingReply = new boolean[n];
        boolean[] waitingWritable = new boolean[n];
        for (int i = 0; i < n; i++) {
            payloads[i] = PerfUtil.payloadTemplate(msgSize);
        }
        List<systems.zlink.contracts.sockets.Socket> socketsAsBase = new ArrayList<>(n);
        for (RouterSocket c : clients) {
            socketsAsBase.add(c);
        }
        int rrIndex = 0;
        // Long-lived Received reused across recv on every client socket. The
        // canonical ref-out recv refills it in place via populateRoutedSinglePart,
        // avoiding the per-recv Received + ArrayList allocation.
        systems.zlink.contracts.messaging.Received replyBuffer = new systems.zlink.contracts.messaging.Received();
        try (PerfSocketPollSet pollSet = PerfSocketPollSet.fromSockets(
                socketsAsBase, PollEventFlags.POLLIN)) {
            long activeEnd = System.nanoTime()
                + (long) durationSeconds * 1_000_000_000L;
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
                long remainingNs = activeEnd - System.nanoTime();
                if (remainingNs <= 0L) {
                    break;
                }
                int waitMs = (int) Math.min(Integer.MAX_VALUE,
                    Math.max(1L, remainingNs / 1_000_000L));
                int readyCount = pollSet.poll(waitMs);
                for (int readyOffset = 0; readyOffset < readyCount;
                     readyOffset++) {
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
                        waitingWritable, msgSize, metrics, pollSet, replyBuffer);
                }
            }
            replyBuffer.close();
            Message.closeAll(List.of(payloads));
            for (int i = 0; i < n; i++) {
                try (Message stop = PerfStopToken.newMessage();
                     PerfSocketPollSet stopPoll = PerfSocketPollSet.fromSockets(
                         List.of(clients.get(i)), PollEventFlags.POLLOUT)) {
                    stopPoll.setEvents(0);
                    sendStopToken(clients.get(i), stopPoll, stop);
                }
            }
        }
        // ctx auto-HWM was already applied at setup; reference kept for the
        // compiler so the parameter isn't flagged unused.
        if (ctx == null) {
            throw new IllegalStateException("ctx must not be null");
        }
    }

    private static void drainReplies(RouterSocket client, int idx,
                                     boolean[] waitingReply,
                                     boolean[] waitingWritable,
                                     int msgSize, PerfUtil.Metrics metrics,
                                     PerfSocketPollSet pollSet,
                                     systems.zlink.contracts.messaging.Received replyBuffer) {
        while (true) {
            boolean ok;
            try {
                ok = client.recv(replyBuffer, RecvFlags.DONT_WAIT);
            } catch (ZlinkRecvException ex) {
                if (ex.getResult() == RecvResult.NO_DATA
                    || ex.getResult() == RecvResult.BUSY) {
                    break;
                }
                throw ex;
            }
            if (!ok) break;
            if (!waitingReply[idx]) {
                replyBuffer.close();
                continue;
            }
            waitingReply[idx] = false;
            PerfUtil.recordActiveLatency(metrics, replyBuffer.firstPart(),
                msgSize, true);
            waitingWritable[idx] = false;
            updatePollMask(pollSet, idx, false, false);
        }
    }

    private static boolean trySendPayload(RouterSocket client, Message payload) {
        return client.send(SERVER_ID)
            .message(payload)
            .flags(SendFlags.DONT_WAIT)
            .submit();
    }

    private static void sendStopToken(RouterSocket client,
                                      PerfSocketPollSet pollSet,
                                      Message stop) {
        while (true) {
            try {
                if (client.send(SERVER_ID)
                    .message(stop)
                    .flags(SendFlags.DONT_WAIT)
                    .submit()) {
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
            pollSet.setEvents(0, PollEventFlags.POLLOUT);
            pollSet.poll(-1);
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

    private static boolean isTransient(ZlinkException ex) {
        if (ex instanceof ZlinkSubmitException submit) {
            return submit.getResult() == SubmitResult.BACKPRESSURED;
        }
        return ex.getNativeErrno() == 11 || ex.getNativeErrno() == 4;
    }
}
