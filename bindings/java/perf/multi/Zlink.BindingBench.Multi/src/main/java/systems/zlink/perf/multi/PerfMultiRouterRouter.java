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
import systems.zlink.perf.PerfSocketPollSet;
import systems.zlink.perf.PerfUtil;
import java.nio.charset.StandardCharsets;
import java.time.Duration;
import java.util.ArrayList;
import java.util.List;
import java.util.concurrent.CompletionStage;
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
             var monitor = server.monitorOpen(
                 config.monitorHwm(), MonitorEventType.CONNECTION_READY)) {
            server.setRoutingId(SERVER_ID);
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
            PerfMultiRoutedRelay.run(server, stopRequested);
            return PerfUtil.Result.silent(config);
        }
    }

    // One context owns all N sockets. One coordinator thread submits each
    // public async admission fairly and owns POLLIN dispatch; completion
    // callbacks only publish that a socket may participate in a later round.
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
                        config.monitorHwm(), MonitorEventType.CONNECTION_READY);
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

                runRouterRouterClientLoop(clients, config, durationSeconds,
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

    // One receive poll set multiplexes all client sockets. Send backpressure
    // remains Core-owned by each public async terminal and never enters this
    // poller's event mask or gates on an echo reply.
    private static void runRouterRouterClientLoop(List<RouterSocket> clients,
                                                  PerfUtil.Config config,
                                                  int durationSeconds,
                                                  PerfUtil.Metrics metrics) {
        int n = clients.size();
        int msgSize = config.size();
        List<systems.zlink.contracts.sockets.Socket> socketsAsBase = new ArrayList<>(n);
        for (RouterSocket c : clients) {
            socketsAsBase.add(c);
        }
        // Long-lived Received reused across recv on every client socket. The
        // canonical ref-out recv refills it in place via populateRoutedSinglePart,
        // avoiding the per-recv Received + ArrayList allocation.
        systems.zlink.contracts.messaging.Received replyBuffer = new systems.zlink.contracts.messaging.Received();
        try (PerfSocketPollSet pollSet = PerfSocketPollSet.fromSockets(
                socketsAsBase, PollEventFlags.POLLIN)) {
            long activeEnd = System.nanoTime()
                + (long) durationSeconds * 1_000_000_000L;
            PerfMultiRoutedSendCoordinator.run(n, activeEnd, pollSet,
                index -> sendPayload(clients.get(index), msgSize, activeEnd),
                index -> drainReplies(clients.get(index), msgSize, metrics,
                    replyBuffer, activeEnd),
                Duration.ofSeconds(durationSeconds + 5L),
                "multi router/router async sends");
            replyBuffer.close();
            // C routed echo ends the relay through the runner control path.
            // Do not inject an extra routed stop frame after active traffic.
        }
    }

    private static void drainReplies(RouterSocket client,
                                     int msgSize, PerfUtil.Metrics metrics,
                                     systems.zlink.contracts.messaging.Received replyBuffer,
                                     long activeEnd) {
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
            long receivedNanoTime = System.nanoTime();
            if (receivedNanoTime >= activeEnd) break;
            Message payload = PerfUtil.measurementPayload(replyBuffer.parts());
            if (payload != null) {
                PerfUtil.recordActiveLatency(metrics, payload, msgSize, true,
                    receivedNanoTime);
            }
        }
    }

    private static CompletionStage<Void> sendPayload(RouterSocket client,
                                                     int msgSize,
                                                     long activeEnd) {
        try (Message payload = PerfUtil.payload(msgSize,
                 (byte) PerfUtil.PHASE_ACTIVE, System.nanoTime());
             Message tail = PerfUtil.measurementPartCount() == 2
                 ? PerfUtil.measurementTail() : null) {
            if (PerfUtil.measurementPartCount() == 2) {
                return client.send(SERVER_ID).message(payload)
                    .message(tail)
                    .timeout(PerfMultiAsyncSendLoop.remainingTimeout(activeEnd))
                    .submit();
            }
            return client.send(SERVER_ID).message(payload)
                .timeout(PerfMultiAsyncSendLoop.remainingTimeout(activeEnd))
                .submit();
        }
    }

}
