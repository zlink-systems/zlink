/* SPDX-License-Identifier: MPL-2.0 */

package systems.zlink.perf.multi;

import systems.zlink.contracts.core.Context;
import systems.zlink.contracts.sockets.DealerSocket;
import systems.zlink.contracts.messaging.Message;
import systems.zlink.contracts.eventing.MonitorEventType;
import systems.zlink.contracts.eventing.SocketMonitor;
import systems.zlink.contracts.eventing.PollEventFlags;
import systems.zlink.contracts.sockets.RouterSocket;
import systems.zlink.contracts.core.RoutingId;
import systems.zlink.contracts.sockets.SocketType;
import systems.zlink.perf.PerfControl;
import systems.zlink.perf.PerfSocketPollSet;
import systems.zlink.perf.PerfUtil;
import java.time.Duration;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.List;
import java.util.concurrent.CompletionStage;
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
             var monitor = server.monitorOpen(
                 config.monitorHwm(), MonitorEventType.CONNECTION_READY)) {
            PerfUtil.applySocketOptions(server, config);
            PerfUtil.configureServerTls(server, config.transport());
            server.bind(config.endpoint());
            PerfControl.emitReady(config.endpoint());
            // The receive loop is the readiness barrier for routed clients.
            // On Windows, a monitor event can be consumed before the
            // aggregate count reaches the expected client count even though
            // the corresponding socket is usable. Waiting for that count
            // makes the case fail before it can measure traffic; the poller
            // below admits each connection while runner control owns
            // termination.
            PerfUtil.recalculateAutoHwm(ctx);
            PerfUtil.printMultiMonitorAutoHwm(config, monitor, "server",
                "server", SocketType.ROUTER);
            PerfMultiRoutedRelay.run(server, stopRequested);
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
                SocketMonitor monitor = client.monitorOpen(
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
        List<systems.zlink.contracts.sockets.Socket> socketsAsBase = new ArrayList<>(n);
        socketsAsBase.addAll(clients);
        systems.zlink.contracts.messaging.Received replyBuffer = new systems.zlink.contracts.messaging.Received();
        try (PerfSocketPollSet pollSet = PerfSocketPollSet.fromSockets(
                socketsAsBase, PollEventFlags.POLLIN)) {
            long activeEnd = System.nanoTime()
                + (long) config.durationSeconds() * 1_000_000_000L;
            PerfMultiTargetCoordinator.run(n, activeEnd, pollSet,
                index -> sendPayload(clients.get(index), msgSize, activeEnd),
                index -> drainReplies(clients.get(index), msgSize, metrics,
                    replyBuffer, activeEnd),
                Duration.ofSeconds(config.durationSeconds() + 5L),
                "multi dealer/router async sends");
            replyBuffer.close();
            // C routed echo ends the relay through the runner control path.
            // Do not inject an extra routed stop frame into the measured
            // topology after the active client phase.
        }
    }

    private static CompletionStage<Void> sendPayload(DealerSocket client,
                                                     int msgSize,
                                                     long activeEnd) {
        try (Message payload = PerfUtil.payload(msgSize,
                 (byte) PerfUtil.PHASE_ACTIVE, System.nanoTime());
             Message tail = PerfUtil.measurementPartCount() == 2
                 ? PerfUtil.measurementTail() : null) {
            if (PerfUtil.measurementPartCount() == 2) {
                return client.send().message(payload).message(tail)
                    .submit();
            }
            return client.send().message(payload)
                .submit();
        }
    }

    private static void drainReplies(DealerSocket client,
                                     int msgSize,
                                     PerfUtil.Metrics metrics,
                                     systems.zlink.contracts.messaging.Received replyBuffer,
                                     long activeEnd) {
        while (true) {
            if (!client.recv(replyBuffer, systems.zlink.contracts.sockets.RecvFlags.DONT_WAIT)) {
                break;
            }
            long receivedNanoTime = System.nanoTime();
            if (receivedNanoTime >= activeEnd) break;
            Message payload = PerfUtil.measurementPayload(replyBuffer.parts());
            if (payload != null) {
                PerfUtil.recordActiveLatency(metrics, payload, msgSize, true,
                    receivedNanoTime);
            }
        }
    }

}
