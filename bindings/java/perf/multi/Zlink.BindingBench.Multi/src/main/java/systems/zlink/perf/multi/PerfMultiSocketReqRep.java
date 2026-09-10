/* SPDX-License-Identifier: MPL-2.0 */

package systems.zlink.perf.multi;

import systems.zlink.contracts.core.Context;
import systems.zlink.contracts.core.RoutingId;
import systems.zlink.contracts.eventing.MonitorEventType;
import systems.zlink.contracts.eventing.PollEventFlags;
import systems.zlink.contracts.eventing.SocketMonitor;
import systems.zlink.contracts.messaging.Message;
import systems.zlink.contracts.messaging.Received;
import systems.zlink.contracts.messaging.RequestSubmission;
import systems.zlink.contracts.sockets.DealerSocket;
import systems.zlink.contracts.sockets.RecvFlags;
import systems.zlink.contracts.sockets.RequestResult;
import systems.zlink.contracts.sockets.RouterSocket;
import systems.zlink.contracts.sockets.Socket;
import systems.zlink.contracts.sockets.SubmitResult;
import systems.zlink.contracts.errors.ZlinkRequestException;
import systems.zlink.perf.PerfControl;
import systems.zlink.perf.PerfSocketPollSet;
import systems.zlink.perf.PerfUtil;

import java.nio.charset.StandardCharsets;
import java.time.Duration;
import java.util.ArrayList;
import java.util.List;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicBoolean;
import java.util.concurrent.atomic.AtomicIntegerArray;
import java.util.concurrent.atomic.AtomicLong;
import java.util.concurrent.atomic.AtomicReference;
import java.util.concurrent.locks.LockSupport;

final class PerfMultiSocketReqRep {
    private static final RoutingId SERVER_RID = RoutingId.from(
        "SERVER".getBytes(StandardCharsets.UTF_8));
    /** C reference perf_aux_poll_wait_ms() (perf_multi_poll.hpp:39). */
    private static final int AUX_POLL_WAIT_MS = 100;
    /**
     * Opt-in submit instrumentation. Off by default so measured runs keep the
     * plain submission and completion callbacks.
     */
    private static final boolean DIAGNOSTICS = diagnosticsEnabled();

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
            // PERF_POLICY.md:481, PERF_MULTI_TEST_POLICY.md:380,383 — the
            // request/reply server shuts down on the runner stdin STOP/QUIT
            // exactly like the C reference
            // (bindings/c/perf/multi/common/perf_multi_socket_reqrep.hpp:1041-1053),
            // not on a wire stop token. The 100 ms wait is the same auxiliary
            // teardown wait C uses (perf_aux_poll_wait_ms(), :970).
            AtomicBoolean stopRequested =
                PerfControl.watchStopSignal("socket reqrep server");
            PerfControl.emitReady(config.endpoint());
            PerfUtil.recalculateAutoHwm(ctx);

            try (PerfSocketPollSet poller = PerfSocketPollSet.fromSockets(
                     List.of(server), PollEventFlags.POLLIN);
                 Received received = new Received()) {
                while (!stopRequested.get()) {
                    int readyCount = poller.poll(AUX_POLL_WAIT_MS);
                    if (readyCount <= 0
                        || !poller.readyHasEventAt(0,
                            PollEventFlags.POLLIN)) {
                        continue;
                    }
                    // The reference C server drains only after POLLIN.  Do
                    // not add an unconditional native DONT_WAIT recv after a
                    // poll wake that belongs to another event class.
                    while (server.recv(received, RecvFlags.DONT_WAIT)) {
                        if (received.replyToken().isEmpty()) {
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
            if (DIAGNOSTICS && !clients.isEmpty()) {
                // Diagnostic only: the applied byte send-HWM and the pending
                // send bytes explain how wide the admission window really is
                // at this payload size.
                PerfUtil.printMultiSocketAutoHwm(config, clients.get(0),
                    "client", "reqrep_client_0",
                    routedClients ? systems.zlink.contracts.sockets.SocketType.ROUTER
                                  : systems.zlink.contracts.sockets.SocketType.DEALER);
            }
            PerfUtil.Result result = metrics.finishMulti(config);
            String resultLine = result.toLine("current");
            if (!resultLine.isEmpty()) {
                System.out.println(resultLine);
            }
            // PERF_MULTI_TEST_POLICY.md:379,386-388 / PERF_POLICY.md:483-486 —
            // emit CLIENT_DONE, keep the request completion target sockets
            // open, and close them only after the runner has stopped the
            // server and sent STOP. C reference:
            // bindings/c/perf/multi/common/perf_multi_socket_reqrep.hpp:792,:722-731.
            PerfControl.emitClientDone(config.size());
            PerfControl.awaitStop("multi socket reqrep client");
            return PerfUtil.Result.silent(config);
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
        AtomicLong pendingAdmissions = new AtomicLong();
        AtomicLong timeouts = new AtomicLong();
        AtomicIntegerArray backpressured = new AtomicIntegerArray(
            clients.size());
        Thread submitThread = Thread.currentThread();
        long activeEnd = System.nanoTime()
            + config.durationSeconds() * 1_000_000_000L;
        int requestTimeoutMs = resolveRequestTimeoutMs();
        Duration timeout = Duration.ofMillis(requestTimeoutMs);
        int clientCount = clients.size();
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
                    if (isExpectedRequestFailure(cause)) {
                        // Reported as TIMEOUTS at the end of the run. The
                        // aggregate never counts a timed-out request.
                        timeouts.incrementAndGet();
                    } else {
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
                LockSupport.unpark(submitThread);
            }
        };

        SubmitDiagnostics diagnostics = DIAGNOSTICS
            ? new SubmitDiagnostics(clientCount, completion) : null;

        // PERF_MULTI_TEST_POLICY.md 1.2: OK immediately leaves the socket
        // eligible for the next sweep. Only BACKPRESSURED makes that socket
        // unavailable, until its exact admitted() stage completes. Reply
        // stages progress independently on the binding runtime.
        try (RequestPayloadTemplates payloadTemplates =
                 new RequestPayloadTemplates(config.size(), clients.size())) {
            while (System.nanoTime() < activeEnd && failure.get() == null) {
                boolean submitted = false;
                for (int i = 0; i < clients.size(); i++) {
                    if (System.nanoTime() >= activeEnd) {
                        break;
                    }
                    if (backpressured.get(i) != 0) {
                        continue;
                    }
                    payloadTemplates.prepare(i, (byte) PerfUtil.PHASE_ACTIVE,
                        System.nanoTime());
                    RequestSubmission submission = submit(clients.get(i),
                        routedClients,
                        payloadTemplates.copyForSubmit(i), timeout,
                        outstanding,
                        diagnostics == null ? completion
                                            : diagnostics.beforeSubmit(i));
                    submitted = true;
                    SubmitResult result = submission.result();
                    if (result == SubmitResult.OK) {
                        continue;
                    }
                    if (result != SubmitResult.BACKPRESSURED) {
                        throw new IllegalStateException(
                            "async request returned " + result);
                    }
                    backpressured.set(i, 1);
                    pendingAdmissions.incrementAndGet();
                    if (diagnostics != null) {
                        diagnostics.recordBackpressure();
                    }
                    int socketIndex = i;
                    submission.admitted().whenComplete((ignored, error) -> {
                        if (error != null) {
                            failure.compareAndSet(null,
                                PerfMultiAsyncSendLoop.completionCause(error));
                        }
                        backpressured.set(socketIndex, 0);
                        pendingAdmissions.decrementAndGet();
                        LockSupport.unpark(submitThread);
                    });
                }

                if (!submitted && failure.get() == null) {
                    long remainingNanos = activeEnd - System.nanoTime();
                    if (remainingNanos > 0L) {
                        // Every socket is BACKPRESSURED. Its admitted()
                        // completion is the only event that resumes submits.
                        LockSupport.parkNanos(PerfMultiSocketReqRep.class,
                            remainingNanos);
                    }
                    if (Thread.currentThread().isInterrupted()) {
                        throw new IllegalStateException(
                            "multi socket reqrep interrupted");
                    }
                }
            }
            long drainEnd = System.nanoTime() + TimeUnit.MILLISECONDS.toNanos(
                Math.max(1_000, requestTimeoutMs * 4));
            while ((outstanding.get() > 0 || pendingAdmissions.get() > 0)
                   && failure.get() == null
                   && System.nanoTime() < drainEnd) {
                LockSupport.parkNanos(PerfMultiSocketReqRep.class,
                    Math.max(1L, drainEnd - System.nanoTime()));
                if (Thread.currentThread().isInterrupted()) {
                    throw new IllegalStateException(
                        "multi socket reqrep drain interrupted");
                }
            }
            if (outstanding.get() != 0 || pendingAdmissions.get() != 0
                || failure.get() != null) {
                System.err.println("TIMEOUTS," + timeouts.get());
                if (diagnostics != null) {
                    diagnostics.report(config, System.err);
                }
                throw new IllegalStateException("multi socket reqrep failed",
                    failure.get());
            }
        }
        // Diagnostic only: the aggregate never sees this line. A timed-out
        // request is dropped from the measurement, so the count states how
        // much of the submitted load never produced a reply.
        System.err.println("TIMEOUTS," + timeouts.get());
        if (diagnostics != null) {
            diagnostics.report(config, System.err);
        }
        // No wire stop token here: PERF_POLICY.md:481 keeps STOP a runner
        // orchestration command, and the C request/reply server terminates on
        // the runner stdin STOP alone.
    }

    /**
     * Opt-in counters that expose the per-socket inflight depth and exact
     * BACKPRESSURED result count without changing the submit loop.
     */
    private static final class SubmitDiagnostics {
        private final java.util.function.BiConsumer<List<Message>, Throwable>[]
            consumers;
        private final AtomicLong[] inflight;
        private final long[] maxInflight;
        private long submits;
        private long backpressured;

        @SuppressWarnings("unchecked")
        private SubmitDiagnostics(int clientCount,
                                  java.util.function.BiConsumer<List<Message>,
                                      Throwable> delegate) {
            consumers = new java.util.function.BiConsumer[clientCount];
            inflight = new AtomicLong[clientCount];
            maxInflight = new long[clientCount];
            for (int i = 0; i < clientCount; i++) {
                AtomicLong slot = new AtomicLong();
                inflight[i] = slot;
                consumers[i] = (parts, error) -> {
                    try {
                        delegate.accept(parts, error);
                    } finally {
                        slot.decrementAndGet();
                    }
                };
            }
        }

        private java.util.function.BiConsumer<List<Message>, Throwable>
            beforeSubmit(int index) {
            long depth = inflight[index].incrementAndGet();
            if (depth > maxInflight[index]) {
                maxInflight[index] = depth;
            }
            submits++;
            return consumers[index];
        }

        private void recordBackpressure() {
            backpressured++;
        }

        private void report(PerfUtil.Config config, java.io.PrintStream out) {
            long deepest = 0;
            long residual = 0;
            for (int i = 0; i < inflight.length; i++) {
                deepest = Math.max(deepest, maxInflight[i]);
                residual += inflight[i].get();
            }
            out.println("REQREP_DIAG,size=" + config.size()
                + ",clients=" + inflight.length
                + ",submits=" + submits
                + ",backpressured=" + backpressured
                + ",max_inflight_per_socket=" + deepest
                + ",residual_inflight=" + residual);
        }
    }

    private static RequestSubmission submit(Socket client,
                                            boolean routedClients,
                                            Message payload,
                                            Duration timeout,
                                            AtomicLong outstanding,
                                            java.util.function.BiConsumer<
                                                List<Message>, Throwable>
                                                completion) {
        RequestSubmission submission;
        try (payload;
             Message tail = PerfUtil.measurementPartCount() == 2
                 ? PerfUtil.measurementTail() : null) {
            if (routedClients) {
                if (tail != null) {
                    submission = ((RouterSocket) client).request(SERVER_RID)
                        .message(payload).message(tail).timeout(timeout)
                        .submit();
                } else {
                    submission = ((RouterSocket) client).request(SERVER_RID)
                        .message(payload).timeout(timeout).submit();
                }
            } else if (tail != null) {
                submission = ((DealerSocket) client).request().message(payload)
                    .message(tail).timeout(timeout).submit();
            } else {
                submission = ((DealerSocket) client).request().message(payload)
                    .timeout(timeout).submit();
            }
        }

        outstanding.incrementAndGet();
        submission.reply().whenComplete(completion);
        return submission;
    }

    /** One native template per requester supplies independently owned submits. */
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
            // The binding may retain this clone through WRITABLE admission;
            // it must not alias a later template rewrite.
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

    private static boolean diagnosticsEnabled() {
        String configured = System.getenv("PERF_MULTI_REQREP_DIAG");
        return configured != null && !configured.isBlank()
            && !configured.equals("0");
    }

    private static int resolveRequestTimeoutMs() {
        String configured = System.getenv("PERF_MULTI_REQREP_TIMEOUT_MS");
        if (configured == null || configured.isBlank()) {
            return 200;
        }
        return Math.max(1, Integer.parseInt(configured));
    }
}
