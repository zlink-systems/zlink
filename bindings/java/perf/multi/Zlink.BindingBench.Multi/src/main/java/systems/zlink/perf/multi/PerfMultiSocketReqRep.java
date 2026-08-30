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
import systems.zlink.contracts.errors.ZlinkRequestException;
import systems.zlink.contracts.errors.ZlinkSubmitException;
import systems.zlink.perf.PerfControl;
import systems.zlink.perf.PerfSocketPollSet;
import systems.zlink.perf.PerfStopToken;
import systems.zlink.perf.PerfUtil;

import java.nio.charset.StandardCharsets;
import java.time.Duration;
import java.util.ArrayList;
import java.util.List;
import java.util.concurrent.CompletionStage;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicLong;
import java.util.concurrent.atomic.AtomicReference;

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
                    config.monitorHwm(), MonitorEventType.CONNECTION_READY);
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
        boolean[] pendingPayloads = new boolean[clients.size()];
        java.util.function.BiConsumer<List<Message>, Throwable> completion =
            (parts, error) -> {
            try {
                long receivedAt = System.nanoTime();
                Message payload = error == null
                    ? PerfUtil.measurementPayload(parts) : null;
                if (error == null && payload != null
                    && receivedAt < activeEnd) {
                    PerfUtil.recordActiveLatency(metrics, payload,
                        config.size(), true, receivedAt);
                } else if (error != null) {
                    Throwable cause = PerfMultiAsyncSendLoop.completionCause(
                        error);
                    if (!isExpectedRequestFailure(cause)) {
                        failure.compareAndSet(null, cause);
                    }
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
        try (RequestPayloadTemplates payloadTemplates =
                 new RequestPayloadTemplates(config.size(), clients.size());
             PerfSocketPollSet completionPoller = PerfSocketPollSet.fromSockets(
                 clients, PollEventFlags.POLLCOMPLETION)) {
            while (System.nanoTime() < activeEnd && failure.get() == null) {
                boolean progress = false;
                for (int i = 0; i < clients.size(); i++) {
                    if (System.nanoTime() >= activeEnd) {
                        break;
                    }
                    if (!pendingPayloads[i]) {
                        payloadTemplates.prepare(i, (byte) PerfUtil.PHASE_ACTIVE,
                            System.nanoTime());
                        pendingPayloads[i] = true;
                    }
                    // Each accepted request owns its own CompletionStage.
                    // Keep submitting round-robin until Core reports
                    // admission backpressure; replies do not gate later
                    // request tasks.
                    if (submit(clients.get(i), routedClients,
                            payloadTemplates.copyForSubmit(i), timeout,
                            outstanding, completion)) {
                        pendingPayloads[i] = false;
                        progress = true;
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

    private static boolean submit(Socket client, boolean routedClients,
                                  Message payload, Duration timeout,
                                  AtomicLong outstanding,
                                  java.util.function.BiConsumer<List<Message>,
                                      Throwable> completion) {
        CompletionStage<List<Message>> stage;
        try (payload;
             Message tail = PerfUtil.measurementPartCount() == 2
                 ? PerfUtil.measurementTail() : null) {
            if (routedClients) {
                if (tail != null) {
                    stage = ((RouterSocket) client).request(SERVER_RID)
                        .message(payload).message(tail).timeout(timeout)
                        .submit();
                } else {
                    stage = ((RouterSocket) client).request(SERVER_RID)
                        .message(payload).timeout(timeout).submit();
                }
            } else if (tail != null) {
                stage = ((DealerSocket) client).request().message(payload)
                    .message(tail).timeout(timeout).submit();
            } else {
                stage = ((DealerSocket) client).request().message(payload)
                    .timeout(timeout).submit();
            }
        } catch (RuntimeException | Error error) {
            Throwable cause = PerfMultiAsyncSendLoop.completionCause(error);
            if (isTransientAdmission(cause)) {
                return false;
            }
            throw error;
        }

        var future = stage.toCompletableFuture();
        if (future.isCompletedExceptionally()) {
            try {
                future.join();
            } catch (java.util.concurrent.CompletionException error) {
                Throwable cause = PerfMultiAsyncSendLoop.completionCause(error);
                if (isTransientAdmission(cause)) {
                    return false;
                }
                if (cause instanceof RuntimeException runtime) {
                    throw runtime;
                }
                if (cause instanceof Error fatal) {
                    throw fatal;
                }
                throw error;
            }
        }
        outstanding.incrementAndGet();
        stage.whenComplete(completion);
        return true;
    }

    /**
     * One native template per requester preserves a pre-admission logical
     * payload across retries. Request submission copies the source parts into
     * Core-owned storage before returning, so its temporary clone is released
     * independently of reply completion; this is not an inflight window.
     */
    private static final class RequestPayloadTemplates implements AutoCloseable {
        private final Message[] templates;

        private RequestPayloadTemplates(int size, int clientCount) {
            templates = new Message[clientCount];
            for (int index = 0; index < clientCount; index++) {
                templates[index] = PerfUtil.payloadTemplate(size);
            }
        }

        private void prepare(int index, byte phase, long sentNanoTime) {
            PerfUtil.writePayload(templates[index], templates[index].size(),
                phase, sentNanoTime);
        }

        private Message copyForSubmit(int index) {
            // CoreRequestSupport copies source parts before it returns. A
            // clone must therefore be independently owned, never shared with
            // a subsequent retry or template rewrite.
            return Message.from(templates[index]);
        }

        @Override
        public void close() {
            Message.closeAll(templates);
        }
    }

    private static boolean isExpectedRequestFailure(Throwable error) {
        return error instanceof ZlinkRequestException request
            && request.getResult() == RequestResult.TIMED_OUT;
    }

    private static boolean isTransientAdmission(Throwable error) {
        return error instanceof ZlinkSubmitException submit
            && (submit.getResult() == SubmitResult.BACKPRESSURED
                || submit.getResult() == SubmitResult.NOT_CONNECTED
                || submit.getResult() == SubmitResult.NOT_ADMITTED);
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
