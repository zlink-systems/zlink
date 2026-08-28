/* SPDX-License-Identifier: MPL-2.0 */

package systems.zlink.perf.multi;

import systems.zlink.contracts.core.Context;
import systems.zlink.contracts.messaging.Message;
import systems.zlink.contracts.eventing.MonitorEventType;
import systems.zlink.contracts.eventing.SocketMonitor;
import systems.zlink.contracts.eventing.PollEventFlags;
import systems.zlink.contracts.errors.ZlinkRecvException;
import systems.zlink.contracts.sockets.RecvFlags;
import systems.zlink.contracts.sockets.RecvResult;
import systems.zlink.contracts.sockets.RouterSocket;
import systems.zlink.contracts.core.RoutingId;
import systems.zlink.contracts.sockets.SocketType;
import systems.zlink.perf.PerfControl;
import systems.zlink.perf.PerfMessageTemplatePool;
import systems.zlink.perf.PerfSocketPollSet;
import systems.zlink.perf.PerfUtil;
import java.nio.charset.StandardCharsets;
import java.time.Duration;
import java.util.ArrayList;
import java.util.List;
import java.util.concurrent.atomic.AtomicBoolean;

final class PerfMultiRouterRouter {
    private static final MonitorEventType READY_EVENT =
        MonitorEventType.CONNECTION_READY;
    private static final RoutingId SERVER_ID = RoutingId.from(
        "PERF_SERVER".getBytes(StandardCharsets.UTF_8));

    private PerfMultiRouterRouter() {
    }

    static PerfUtil.Result runServer(PerfUtil.Config config) {
        AtomicBoolean stopRequested = PerfControl.watchStopSignal("router-router server");
        try (Context ctx = PerfUtil.newContext(config);
             RouterSocket server = ctx.createRouterSocket();
             var monitor = server.monitorOpen(MonitorEventType.CONNECTION_READY)) {
            server.setRoutingId(SERVER_ID);
            PerfUtil.applyMonitorOptions(monitor, config);
            PerfUtil.applySocketOptions(server, config);
            PerfUtil.configureServerTls(server, config.transport());
            server.bind(config.endpoint());
            PerfControl.emitReady(config.endpoint());
            // The routed receive loop admits connections while it waits for
            // traffic. An aggregate CONNECTION_READY monitor count is not a
            // reliable Windows barrier for many sockets, so the loop itself
            // provides readiness while runner control owns termination.
            PerfUtil.recalculateAutoHwm(ctx);
            PerfUtil.printMultiMonitorAutoHwm(config, monitor, "server",
                "server", SocketType.ROUTER);
            // Long-lived caller-provided Received reused across every recv on
            // the server hot path. The binding refills its internal state in
            // place via adoptFrom, avoiding the per-recv Received allocation
            // that the legacy `recv() -> Received` path forced. Matches the
            // C++/.NET canonical caller-provided storage pattern.
            systems.zlink.contracts.messaging.Received receivedBuffer = new systems.zlink.contracts.messaging.Received();
            try (PerfSocketPollSet pollSet = PerfSocketPollSet.fromSockets(
                List.of(server), PollEventFlags.POLLIN)) {
                // C terminates this relay from runner stdin. Client stop
                // tokens remain regular routed frames and must be echoed.
                while (!stopRequested.get()) {
                    pollSet.setEvents(0, PollEventFlags.POLLIN);
                    int readyCount = pollSet.poll(50);
                    boolean readable = readyCount > 0
                        && pollSet.readyHasEventAt(0, PollEventFlags.POLLIN);
                    if (!readable) {
                        continue;
                    }
                    while (true) {
                        if (stopRequested.get()) {
                            break;
                        }
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

                        RoutingId rid = receivedBuffer.getRoutingId().orElseThrow();
                        Message payload = PerfUtil.measurementPayload(receivedBuffer.parts());
                        if (payload == null) {
                            receivedBuffer.close();
                            continue;
                        }
                        Message ownedReply = Message.from(payload);
                        receivedBuffer.close();
                        try (ownedReply) {
                            if (PerfUtil.measurementPartCount() == 2) {
                                PerfUtil.awaitStage(server.send(rid).message(ownedReply)
                                    .message(PerfUtil.measurementTail()).submit());
                            } else {
                                PerfUtil.awaitStage(server.send(rid).message(ownedReply).submit());
                            }
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
                Duration readyTimeout = Duration.ofMillis(
                    config.connectReadyTimeoutMs());
                for (SocketMonitor monitor : monitors) {
                    PerfUtil.waitForMonitorEvent(monitor, READY_EVENT, 1,
                        readyTimeout, "router/router client ready");
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
        boolean[] waitingReply = new boolean[n];
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
                socketsAsBase, PollEventFlags.POLLIN);
             PerfMessageTemplatePool payloadPool = new PerfMessageTemplatePool(
                 msgSize, Math.max(4, n * 2))) {
            long activeEnd = System.nanoTime()
                + (long) durationSeconds * 1_000_000_000L;
            while (System.nanoTime() < activeEnd) {
                int startIndex = rrIndex;
                for (int i = 0; i < n; i++) {
                    int idx = (startIndex + i) % n;
                    if (waitingReply[idx]) continue;
                    Message payload = payloadPool.acquire(msgSize,
                        (byte) PerfUtil.PHASE_ACTIVE, System.nanoTime(),
                        activeEnd);
                    if (payload == null) {
                        break;
                    }
                    try {
                        sendPayload(clients.get(idx), payload);
                        waitingReply[idx] = true;
                    } finally {
                        payload.close();
                    }
                }
                rrIndex = (startIndex + 1) % n;
                // The requester owns the active deadline. Bound the wait by
                // its remaining interval so a quiet relay cannot keep this
                // phase past the configured duration.
                int readyCount = pollSet.poll(remainingTimeoutMs(activeEnd));
                for (int readyOffset = 0; readyOffset < readyCount;
                     readyOffset++) {
                    int idx = pollSet.readyIndexAt(readyOffset);
                    boolean readable =
                        pollSet.readyHasEventAt(readyOffset,
                            PollEventFlags.POLLIN);
                    if (!readable) continue;
                    drainReplies(clients.get(idx), idx, waitingReply,
                        msgSize, metrics, replyBuffer);
                }
            }
            replyBuffer.close();
            // C routed echo ends the relay through the runner control path.
            // Do not inject an extra routed stop frame after active traffic.
        }
        // ctx auto-HWM was already applied at setup; reference kept for the
        // compiler so the parameter isn't flagged unused.
        if (ctx == null) {
            throw new IllegalStateException("ctx must not be null");
        }
    }

    private static void drainReplies(RouterSocket client, int idx,
                                     boolean[] waitingReply,
                                     int msgSize, PerfUtil.Metrics metrics,
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
            Message payload = PerfUtil.measurementPayload(replyBuffer.parts());
            if (payload != null) {
                PerfUtil.recordActiveLatency(metrics, payload, msgSize, true);
            }
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

    private static void sendPayload(RouterSocket client, Message payload) {
        if (PerfUtil.measurementPartCount() == 2) {
            PerfUtil.awaitStage(client.send(SERVER_ID).message(payload)
                .message(PerfUtil.measurementTail()).submit());
        } else {
            PerfUtil.awaitStage(client.send(SERVER_ID).message(payload).submit());
        }
    }
}
