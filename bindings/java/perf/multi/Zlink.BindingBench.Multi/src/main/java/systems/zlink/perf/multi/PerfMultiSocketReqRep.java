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
import systems.zlink.contracts.errors.ZlinkRequestException;
import systems.zlink.contracts.errors.ZlinkSubmitException;
import systems.zlink.perf.PerfControl;
import systems.zlink.perf.PerfMessageTemplatePool;
import systems.zlink.perf.PerfSocketPollSet;
import systems.zlink.perf.PerfStopToken;
import systems.zlink.perf.PerfUtil;

import java.nio.charset.StandardCharsets;
import java.time.Duration;
import java.util.ArrayList;
import java.util.List;
import java.util.concurrent.CompletionException;
import java.util.concurrent.Semaphore;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicBoolean;
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
                        if (PerfStopToken.isStopTokenMessage(received.firstPart())) {
                            stops++;
                            received.close();
                            continue;
                        }
                        if (received.requestSeq().isEmpty()) {
                            received.close();
                            continue;
                        }
                        received.reply().message(received.firstPart()).submit();
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
        int count = clients.size();
        AtomicBoolean[] waiting = new AtomicBoolean[count];
        AtomicReference<Throwable> failure = new AtomicReference<>();
        Semaphore completionSignals = new Semaphore(0);
        long activeEnd = System.nanoTime()
            + config.durationSeconds() * 1_000_000_000L;
        int requestTimeoutMs = resolveRequestTimeoutMs();
        Duration timeout = Duration.ofMillis(requestTimeoutMs);
        @SuppressWarnings("unchecked")
        BiConsumer<List<Message>, Throwable>[] completions =
            (BiConsumer<List<Message>, Throwable>[]) new BiConsumer<?, ?>[count];
        for (int i = 0; i < count; i++) {
            waiting[i] = new AtomicBoolean();
            AtomicBoolean slotWaiting = waiting[i];
            completions[i] = (parts, error) -> {
                try {
                    long receivedAt = System.nanoTime();
                    Throwable cause = completionCause(error);
                    if (cause == null && parts != null
                        && !parts.isEmpty() && receivedAt < activeEnd) {
                        PerfUtil.recordActiveLatency(metrics, parts.get(0),
                            config.size(), true, receivedAt);
                    } else if (cause != null
                        && (!(cause instanceof ZlinkRequestException request)
                            || request.getResult() != RequestResult.TIMED_OUT)) {
                        failure.compareAndSet(null, cause);
                    }
                } catch (Throwable ex) {
                    failure.compareAndSet(null, ex);
                } finally {
                    if (parts != null) {
                        Message.closeAll(parts);
                    }
                    slotWaiting.set(false);
                    completionSignals.release();
                }
            };
        }

        // Keep reusable native payload storage per concurrent request slot.
        // acquire() returns an independent Message owner, so request submit
        // snapshots it before returning exactly as the public contract
        // requires, while the template is reused after Core releases it.
        try (PerfMessageTemplatePool payloads = new PerfMessageTemplatePool(
                config.size(), Math.max(4, count * 2))) {
            while (System.nanoTime() < activeEnd && failure.get() == null) {
                boolean progress = false;
                boolean hasWaiting = false;
                for (int i = 0; i < count; i++) {
                    if (waiting[i].get()) {
                        hasWaiting = true;
                        continue;
                    }
                    Message payload = payloads.acquire(config.size(),
                        (byte) PerfUtil.PHASE_ACTIVE, System.nanoTime(),
                        activeEnd);
                    if (payload == null) {
                        break;
                    }
                    waiting[i].set(true);
                    try (payload) {
                        submit(clients.get(i), routedClients, payload, timeout,
                            completions[i]);
                        progress = true;
                    } catch (ZlinkSubmitException ex) {
                        waiting[i].set(false);
                        if (ex.getResult() != SubmitResult.BACKPRESSURED
                            && ex.getResult() != SubmitResult.NOT_CONNECTED) {
                            throw ex;
                        }
                    }
                }
                if (!progress && hasWaiting) {
                    awaitCompletionSignal(completionSignals, 50);
                }
            }
            long drainEnd = System.nanoTime() + TimeUnit.MILLISECONDS.toNanos(
                Math.max(1_000, requestTimeoutMs * 4));
            while (anyWaiting(waiting) && System.nanoTime() < drainEnd) {
                awaitCompletionSignal(completionSignals, 50);
            }
            if (anyWaiting(waiting) || failure.get() != null) {
                throw new IllegalStateException("multi socket reqrep failed",
                    failure.get());
            }
        }

        for (Socket client : clients) {
            PerfStopToken.sendWithRetry(() -> {
                try (Message stop = PerfStopToken.newMessage()) {
                    if (routedClients) {
                        PerfUtil.awaitStage(((RouterSocket) client)
                            .send(SERVER_RID).message(stop).submit());
                        return true;
                    }
                    PerfUtil.awaitStage(((DealerSocket) client)
                        .send().message(stop).submit());
                    return true;
                }
            }, "multi socket reqrep");
        }
    }

    private static void submit(Socket client, boolean routedClients,
                               Message payload, Duration timeout,
                               BiConsumer<List<Message>, Throwable> completion) {
        var stage = routedClients
            ? ((RouterSocket) client).request(SERVER_RID)
                .message(payload).timeout(timeout).submit()
            : ((DealerSocket) client).request()
                .message(payload).timeout(timeout).submit();
        stage.whenComplete(completion);
    }

    private static Throwable completionCause(Throwable error) {
        if (error instanceof CompletionException completion
            && completion.getCause() != null) {
            return completion.getCause();
        }
        return error;
    }

    private static void awaitCompletionSignal(Semaphore completions,
                                              long timeoutMs) {
        try {
            completions.tryAcquire(timeoutMs, TimeUnit.MILLISECONDS);
        } catch (InterruptedException ex) {
            Thread.currentThread().interrupt();
            throw new IllegalStateException(
                "request completion wait interrupted", ex);
        }
    }

    private static boolean anyWaiting(AtomicBoolean[] waiting) {
        for (AtomicBoolean slot : waiting) {
            if (slot.get()) {
                return true;
            }
        }
        return false;
    }

    private static int resolveRequestTimeoutMs() {
        String configured = System.getenv("PERF_MULTI_REQREP_TIMEOUT_MS");
        if (configured == null || configured.isBlank()) {
            return 200;
        }
        return Math.max(1, Integer.parseInt(configured));
    }
}
