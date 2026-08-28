/* SPDX-License-Identifier: MPL-2.0 */

package systems.zlink.perf.multi;

import systems.zlink.contracts.core.Context;
import systems.zlink.contracts.core.RoutingId;
import systems.zlink.contracts.eventing.MonitorEventType;
import systems.zlink.contracts.eventing.PollEventFlags;
import systems.zlink.contracts.eventing.SocketMonitor;
import systems.zlink.contracts.messaging.Message;
import systems.zlink.contracts.messaging.Received;
import systems.zlink.contracts.sockets.DealerSocket;
import systems.zlink.contracts.sockets.RecvFlags;
import systems.zlink.contracts.sockets.RequestResult;
import systems.zlink.contracts.sockets.RouterSocket;
import systems.zlink.contracts.sockets.Socket;
import systems.zlink.contracts.sockets.SubmitResult;
import systems.zlink.contracts.sockets.SendFlags;
import systems.zlink.contracts.errors.ZlinkSubmitException;
import systems.zlink.perf.PerfControl;
import systems.zlink.perf.PerfSocketPollSet;
import systems.zlink.perf.PerfStopToken;
import systems.zlink.perf.PerfUtil;

import java.nio.charset.StandardCharsets;
import java.time.Duration;
import java.util.ArrayList;
import java.util.List;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicLong;
import java.util.concurrent.atomic.AtomicReference;
import java.util.function.BiConsumer;

final class PerfMultiSocketReqRep {
    private static final RoutingId SERVER_RID = RoutingId.from(
        "SERVER".getBytes(StandardCharsets.UTF_8));

    private PerfMultiSocketReqRep() {
    }

    static PerfUtil.Result runServer(PerfUtil.Config config) {
        try (Context ctx = PerfUtil.newContext(config);
             RouterSocket server = ctx.createRouterSocket()) {
            server.setRoutingId(SERVER_RID);
            server.options().mandatory(true);
            PerfUtil.applySocketOptions(server, config);
            PerfUtil.configureServerTls(server, config.transport());
            server.bind(config.endpoint());
            PerfControl.emitReady(config.endpoint());
            PerfUtil.recalculateAutoHwm(ctx);

            int stops = 0;
            try (PerfSocketPollSet poller = PerfSocketPollSet.fromSockets(
                     List.of(server), PollEventFlags.POLLIN);
                 Received received = new Received()) {
                while (stops < config.clients()) {
                    int readyCount = poller.poll(-1);
                    if (readyCount <= 0
                        || !poller.readyHasEventAt(0,
                            PollEventFlags.POLLIN)) {
                        continue;
                    }
                    // The reference C server drains only after POLLIN.  Do
                    // not add an unconditional native DONT_WAIT recv after a
                    // poll wake that belongs to another event class.
                    while (server.recv(received, RecvFlags.DONT_WAIT)) {
                        if (received.parts().size() == 1
                            && PerfStopToken.isStopTokenMessage(received.firstPart())) {
                            stops++;
                            received.close();
                            continue;
                        }
                        if (received.requestSeq().isEmpty()) {
                            received.close();
                            continue;
                        }
                        Message payload = PerfUtil.measurementPayload(received.parts());
                        if (payload == null) {
                            received.close();
                            continue;
                        }
                        if (PerfUtil.measurementPartCount() == 2) {
                            received.reply().message(payload).message(PerfUtil.measurementTail()).submit();
                        } else {
                            received.reply().message(payload).submit();
                        }
                        received.close();
                    }
                }
            }
            return PerfUtil.Result.silent(config);
        }
    }

    static PerfUtil.Result runClient(PerfUtil.Config config,
                                     boolean routedClients) {
        PerfUtil.Metrics metrics = new PerfUtil.Metrics(config);
        List<Socket> clients = new ArrayList<>(config.clients());
        List<SocketMonitor> monitors = new ArrayList<>(config.clients());
        Context ctx = PerfUtil.newContext(config);
        try {
            for (int i = 0; i < config.clients(); i++) {
                Socket client = routedClients ? ctx.createRouterSocket()
                                              : ctx.createDealerSocket();
                if (client instanceof RouterSocket router) {
                    byte[] rid = ("CLIENT-" + i).getBytes(StandardCharsets.UTF_8);
                    router.setRoutingId(RoutingId.from(rid));
                    router.options().setConnectRoutingId(SERVER_RID);
                    router.options().mandatory(true);
                } else {
                    byte[] rid = ("CLIENT-" + i).getBytes(StandardCharsets.UTF_8);
                    ((DealerSocket) client).setRoutingId(RoutingId.from(rid));
                }
                SocketMonitor monitor = client.monitorOpen(
                    MonitorEventType.CONNECTION_READY);
                PerfUtil.applyMonitorOptions(monitor, config);
                PerfUtil.applySocketOptions(client, config);
                PerfUtil.configureClientTls(client, config.transport());
                if (client instanceof RouterSocket router) {
                    router.connect(config.endpoint());
                } else {
                    ((DealerSocket) client).connect(config.endpoint());
                }
                clients.add(client);
                monitors.add(monitor);
            }
            Duration readyTimeout = Duration.ofMillis(config.connectReadyTimeoutMs());
            for (SocketMonitor monitor : monitors) {
                PerfUtil.waitForMonitorEvent(monitor,
                    MonitorEventType.CONNECTION_READY, 1, readyTimeout,
                    "socket reqrep client ready");
                monitor.close();
            }
            monitors.clear();
            PerfUtil.recalculateAutoHwm(ctx);
            runClients(clients, config, routedClients, metrics);
            return metrics.finishMulti(config);
        } finally {
            for (SocketMonitor monitor : monitors) {
                try {
                    monitor.close();
                } catch (Exception ignored) {
                }
            }
            for (Socket client : clients) {
                try {
                    client.close();
                } catch (Exception ignored) {
                }
            }
            ctx.close();
        }
    }

    private static void runClients(List<Socket> clients,
                                   PerfUtil.Config config,
                                   boolean routedClients,
                                   PerfUtil.Metrics metrics) {
        AtomicReference<Throwable> failure = new AtomicReference<>();
        AtomicLong outstanding = new AtomicLong();
        long activeEnd = System.nanoTime()
            + config.durationSeconds() * 1_000_000_000L;
        int requestTimeoutMs = resolveRequestTimeoutMs();
        Duration timeout = Duration.ofMillis(requestTimeoutMs);
        BiConsumer<RequestResult, List<Message>> completion = (result, parts) -> {
            try {
                long receivedAt = System.nanoTime();
                Message payload = PerfUtil.measurementPayload(parts);
                if (result == RequestResult.OK && payload != null
                    && receivedAt < activeEnd) {
                    PerfUtil.recordActiveLatency(metrics, payload,
                        config.size(), true, receivedAt);
                } else if (result != RequestResult.OK
                    && result != RequestResult.TIMED_OUT) {
                    failure.compareAndSet(null, new IllegalStateException(
                        "request completion failed: " + result));
                }
            } catch (Throwable ex) {
                failure.compareAndSet(null, ex);
            } finally {
                if (parts != null) {
                    Message.closeAll(parts);
                }
                outstanding.decrementAndGet();
            }
        };

        // Request completion targets are registered with POLLCOMPLETION
        // alone. Submission continues until the public request terminal
        // reports backpressure; replies never gate the next request.
        try (PerfSocketPollSet completionPoller = PerfSocketPollSet.fromSockets(
                clients, PollEventFlags.POLLCOMPLETION)) {
            while (System.nanoTime() < activeEnd && failure.get() == null) {
                boolean progress = false;
                for (Socket client : clients) {
                    if (System.nanoTime() >= activeEnd) {
                        break;
                    }
                    // Submit at most one request per socket in a round. A
                    // per-socket inner loop can keep the first socket busy
                    // until the active deadline when its HWM is large, which
                    // starves POLLCOMPLETION dispatch and records no replies.
                    try (Message payload = PerfUtil.payload(config.size(),
                             (byte) PerfUtil.PHASE_ACTIVE,
                             System.nanoTime())) {
                        outstanding.incrementAndGet();
                        try {
                            submit(client, routedClients, payload, timeout,
                                completion);
                            progress = true;
                        } catch (ZlinkSubmitException ex) {
                            outstanding.decrementAndGet();
                            if (ex.getResult() != SubmitResult.BACKPRESSURED
                                && ex.getResult()
                                    != SubmitResult.NOT_CONNECTED) {
                                throw ex;
                            }
                        }
                    }
                }

                // Completion callbacks are dispatched only by this poller.
                // Drain it non-blocking after every submission round, and
                // wait briefly only when every socket was backpressured.
                completionPoller.poll(0);
                if (!progress && System.nanoTime() < activeEnd) {
                    completionPoller.poll(Math.min(50,
                        remainingTimeoutMs(activeEnd)));
                }
            }
            long drainEnd = System.nanoTime() + TimeUnit.MILLISECONDS.toNanos(
                Math.max(1_000, requestTimeoutMs * 4));
            while (outstanding.get() > 0 && System.nanoTime() < drainEnd) {
                completionPoller.poll(remainingTimeoutMs(drainEnd));
            }
            if (outstanding.get() != 0 || failure.get() != null) {
                throw new IllegalStateException("multi socket reqrep failed",
                    failure.get());
            }
        }

        for (Socket client : clients) {
            PerfStopToken.sendWithRetry(() -> {
                try (Message stop = PerfStopToken.newMessage()) {
                    if (routedClients) {
                        ((RouterSocket) client).send(SERVER_RID).message(stop)
                            .submit_sync(SendFlags.NONE);
                        return true;
                    }
                    ((DealerSocket) client).send().message(stop)
                        .submit_sync(SendFlags.NONE);
                    return true;
                }
            }, "multi socket reqrep");
        }
    }

    private static void submit(Socket client, boolean routedClients,
                               Message payload, Duration timeout,
                               BiConsumer<RequestResult, List<Message>> completion) {
        if (routedClients) {
            if (PerfUtil.measurementPartCount() == 2) {
                ((RouterSocket) client).request(SERVER_RID).message(payload)
                    .message(PerfUtil.measurementTail()).timeout(timeout)
                    .submit_sync(SendFlags.DONT_WAIT, completion);
            } else {
                ((RouterSocket) client).request(SERVER_RID).message(payload)
                    .timeout(timeout)
                    .submit_sync(SendFlags.DONT_WAIT, completion);
            }
        } else if (PerfUtil.measurementPartCount() == 2) {
            ((DealerSocket) client).request().message(payload)
                .message(PerfUtil.measurementTail()).timeout(timeout)
                .submit_sync(SendFlags.DONT_WAIT, completion);
        } else {
            ((DealerSocket) client).request().message(payload).timeout(timeout)
                .submit_sync(SendFlags.DONT_WAIT, completion);
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

    private static int resolveRequestTimeoutMs() {
        String configured = System.getenv("PERF_MULTI_REQREP_TIMEOUT_MS");
        if (configured == null || configured.isBlank()) {
            return 200;
        }
        return Math.max(1, Integer.parseInt(configured));
    }
}
