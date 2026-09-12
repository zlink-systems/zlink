/* SPDX-License-Identifier: MPL-2.0 */

package systems.zlink.perf.single;

import systems.zlink.contracts.core.Context;
import systems.zlink.contracts.core.RoutingId;
import systems.zlink.contracts.eventing.PollEventFlags;
import systems.zlink.contracts.messaging.Message;
import systems.zlink.contracts.messaging.Received;
import systems.zlink.contracts.messaging.RequestSubmission;
import systems.zlink.contracts.messaging.SendSubmission;
import systems.zlink.contracts.sockets.DealerSocket;
import systems.zlink.contracts.sockets.RecvFlags;
import systems.zlink.contracts.sockets.RequestResult;
import systems.zlink.contracts.sockets.RouterSocket;
import systems.zlink.contracts.sockets.Socket;
import systems.zlink.contracts.sockets.SubmitResult;
import systems.zlink.contracts.errors.ZlinkException;
import systems.zlink.contracts.errors.ZlinkRecvException;
import systems.zlink.contracts.errors.ZlinkRequestException;
import systems.zlink.perf.PerfStopToken;
import systems.zlink.perf.PerfErrno;
import systems.zlink.perf.PerfSocketPollSet;
import systems.zlink.perf.PerfUtil;

import java.nio.charset.StandardCharsets;
import java.time.Duration;
import java.util.List;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicBoolean;
import java.util.concurrent.atomic.AtomicLong;
import java.util.concurrent.atomic.AtomicReference;

final class PerfSocketReqRep {
    private static final RoutingId SERVER_RID = RoutingId.from(
        "SERVER".getBytes(StandardCharsets.UTF_8));
    private static final RoutingId CLIENT_RID = RoutingId.from(
        "CLIENT".getBytes(StandardCharsets.UTF_8));
    private static final RoutingId DEALER_RID = RoutingId.from(
        "DEALER-REQ".getBytes(StandardCharsets.UTF_8));

    private PerfSocketReqRep() {
    }

    static PerfUtil.Result run(PerfUtil.Config config, boolean routedClient) {
        String endpoint = PerfUtil.endpoint(config.transport(),
            routedClient ? "single-router-router-reqrep"
                         : "single-dealer-router-reqrep");
        PerfUtil.Metrics metrics = new PerfUtil.Metrics(config);
        AtomicBoolean serverStopped = new AtomicBoolean();
        AtomicReference<Throwable> failure = new AtomicReference<>();
        try (Context ctx = PerfUtil.newContext(config);
             RouterSocket server = ctx.createRouterSocket();
             Socket client = routedClient ? ctx.createRouterSocket()
                                          : ctx.createDealerSocket();
             var serverMonitor = server.monitorOpen(
                 config.monitorHwm(),
                 systems.zlink.contracts.eventing.MonitorEventType.CONNECTION_READY);
             var clientMonitor = client.monitorOpen(
                 config.monitorHwm(),
                 systems.zlink.contracts.eventing.MonitorEventType.CONNECTION_READY)) {
            if (client instanceof RouterSocket router) {
                server.setRoutingId(SERVER_RID);
                server.options().mandatory(true);
                router.setRoutingId(CLIENT_RID);
                router.options().mandatory(true);
            } else {
                ((DealerSocket) client).setRoutingId(DEALER_RID);
            }
            PerfUtil.applySocketOptions(server, config);
            PerfUtil.applySocketOptions(client, config);
            PerfUtil.configureServerTls(server, config.transport());
            PerfUtil.configureClientTls(client, config.transport());
            server.bind(PerfUtil.bindEndpoint(endpoint, config.transport()));
            String connectedEndpoint = PerfUtil.connectedEndpoint(server, endpoint,
                config.transport());
            if (client instanceof RouterSocket router) {
                router.connect(connectedEndpoint);
            } else {
                ((DealerSocket) client).connect(connectedEndpoint);
            }
            Duration readyTimeout = Duration.ofMillis(config.connectReadyTimeoutMs());
            PerfUtil.waitForMonitorEventWithActivity(serverMonitor, server,
                systems.zlink.contracts.eventing.MonitorEventType.CONNECTION_READY,
                1, readyTimeout, "reqrep server ready");
            if (client instanceof RouterSocket) {
                PerfUtil.waitForMonitorEventWithActivity(clientMonitor, client,
                    systems.zlink.contracts.eventing.MonitorEventType.CONNECTION_READY,
                    1, readyTimeout, "reqrep client ready");
            } else {
                PerfUtil.waitForMonitorEvent(clientMonitor,
                    systems.zlink.contracts.eventing.MonitorEventType.CONNECTION_READY,
                    1, readyTimeout, "reqrep client ready");
            }
            PerfUtil.recalculateAutoHwm(ctx);
            if (client instanceof RouterSocket router) {
                Duration handshakeTimeout = Duration.ofMillis(Math.max(1,
                    PerfUtil.intEnv("PERF_ROUTER_HANDSHAKE_TIMEOUT_MS", 3000)));
                PerfRouterRouter.performRouterRouterHandshake(server, router,
                    SERVER_RID, CLIENT_RID, handshakeTimeout);
            }

            int completionDrainTimeoutMs = Math.max(1, PerfUtil.intEnv(
                "PERF_SINGLE_REQREP_DRAIN_TIMEOUT_MS", 10_000));
            long admissionWindow = resolveAdmissionWindow(client,
                clientMonitor.status().autoHwmAppliedSendHwmBytes(),
                Math.max(config.size(), PerfUtil.HEADER_SIZE));
            Thread serverThread = new Thread(() -> runServer(server, serverStopped,
                failure),
                "single-socket-reqrep-server");
            serverThread.start();

            long activeEnd = System.nanoTime()
                + config.durationSeconds() * 1_000_000_000L;
            int requestTimeoutMs = Math.max(1,
                PerfUtil.intEnv("PERF_SINGLE_REQREP_TIMEOUT_MS", 200));
            Duration requestTimeout = Duration.ofMillis(requestTimeoutMs);
            runRequestPhase(client, routedClient, config, metrics, failure,
                activeEnd, requestTimeout, completionDrainTimeoutMs,
                admissionWindow);
            sendStop(client, routedClient);
            PerfUtil.join(serverThread, "socket reqrep server",
                Duration.ofSeconds(10));
            if (!serverStopped.get() || failure.get() != null) {
                throw new IllegalStateException("socket reqrep failed",
                    failure.get());
            }
            return metrics.finishSingle(config);
        }
    }

    /**
     * Requester flow of PERF_SINGLE_TEST_POLICY.md 1.1.2/1.1.3.
     *
     * <p>This method runs on the dedicated requester thread. It submits
     * continuously without waiting for replies and drives reply completions
     * itself through one public {@code POLLCOMPLETION} poller. The un-settled
     * set is bounded by the socket's applied HWM admission window, not by a
     * fixed request count.</p>
     *
     * <p>Throughput and latency are anchored exactly as in the C reference
     * (bindings/c/perf/single/common/perf_single_reqrep.hpp
     * record_request_completion): a round trip counts only when its completion
     * lands before the active deadline, and the latency sample is
     * {@code completion nanoTime - header sent_ts_ns}.</p>
     */
    private static long resolveAdmissionWindow(Socket client,
                                               long appliedSendHwmBytes,
                                               int wireSize) {
        long hwmBytes = appliedSendHwmBytes;
        if (hwmBytes == 0L) {
            hwmBytes = client.options().sendHwm();
        }
        if (hwmBytes == 0L) {
            throw new IllegalStateException(
                "requester socket reports no send HWM");
        }
        return Math.max(1L, hwmBytes / Math.max(1, wireSize));
    }

    private static void runRequestPhase(Socket client, boolean routedClient,
                                        PerfUtil.Config config,
                                        PerfUtil.Metrics metrics,
                                        AtomicReference<Throwable> failure,
                                        long activeEnd, Duration requestTimeout,
                                        int completionDrainTimeoutMs,
                                        long admissionWindow) {
        AtomicLong outstanding = new AtomicLong();
        java.util.function.BiConsumer<List<Message>, Throwable> completion =
            (parts, error) -> {
            try {
                long receivedAt = System.nanoTime();
                if (error != null) {
                    Throwable cause = completionCause(error);
                    if (!isExpectedRequestFailure(cause)) {
                        failure.compareAndSet(null, cause);
                    }
                    return;
                }
                Message payload = PerfUtil.measurementPayload(parts);
                if (payload != null && receivedAt < activeEnd) {
                    PerfUtil.Header header = PerfUtil.decodeHeader(
                        payload, config.size(), receivedAt);
                    if (header != null
                        && header.phase() == PerfUtil.PHASE_ACTIVE) {
                        metrics.recordNanos(header.latencyNanos());
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

        try (PerfSocketPollSet completionPoller = PerfSocketPollSet.fromSockets(
                 List.of(client), PollEventFlags.POLLCOMPLETION)) {
            while (System.nanoTime() < activeEnd && failure.get() == null) {
                int submittedSinceProgress = 0;
                while (System.nanoTime() < activeEnd
                       && failure.get() == null
                       && outstanding.get() < admissionWindow) {
                    RequestSubmission submission = submitRequest(client,
                        routedClient, config, requestTimeout, outstanding,
                        completion);
                    SubmitResult result = submission.result();
                    if (result != SubmitResult.OK
                        && result != SubmitResult.BACKPRESSURED) {
                        throw new IllegalStateException(
                            "async request returned " + result);
                    }
                    // Match the C request cursor: keep the submission burst
                    // bounded while letting this same thread drain replies.
                    if (++submittedSinceProgress >= 64) {
                        submittedSinceProgress = 0;
                        completionPoller.poll(0);
                    }
                }

                completionPoller.poll(Math.min(50,
                    remainingTimeoutMs(activeEnd)));
            }

            long drainEnd = System.nanoTime()
                + TimeUnit.MILLISECONDS.toNanos(completionDrainTimeoutMs);
            while (outstanding.get() > 0 && failure.get() == null
                   && System.nanoTime() < drainEnd) {
                completionPoller.poll(remainingTimeoutMs(drainEnd));
            }
            if (outstanding.get() != 0) {
                failure.compareAndSet(null, new IllegalStateException(
                    "single socket reqrep completions did not drain"));
            }
        }
    }

    private static RequestSubmission submitRequest(
            Socket client, boolean routedClient, PerfUtil.Config config,
            Duration timeout, AtomicLong outstanding,
            java.util.function.BiConsumer<List<Message>, Throwable> completion) {
        RequestSubmission submission;
        try (Message request = PerfUtil.payload(config.size(),
                 (byte) PerfUtil.PHASE_ACTIVE, System.nanoTime());
             Message tail = PerfUtil.measurementPartCount() == 2
                 ? PerfUtil.measurementTail() : null) {
            if (routedClient) {
                var operation = ((RouterSocket) client).request(SERVER_RID)
                    .message(request);
                if (tail != null) {
                    operation = operation.message(tail);
                }
                submission = operation.timeout(timeout).submit();
            } else {
                var operation = ((DealerSocket) client).request()
                    .message(request);
                if (tail != null) {
                    operation = operation.message(tail);
                }
                submission = operation.timeout(timeout).submit();
            }
        }

        outstanding.incrementAndGet();
        submission.reply().whenComplete(completion);
        return submission;
    }

    private static Throwable completionCause(Throwable error) {
        Throwable cause = error;
        while ((cause instanceof java.util.concurrent.CompletionException
                || cause instanceof java.util.concurrent.ExecutionException)
               && cause.getCause() != null) {
            cause = cause.getCause();
        }
        return cause;
    }

    private static boolean isExpectedRequestFailure(Throwable error) {
        return error instanceof ZlinkRequestException request
            && request.getResult() == RequestResult.TIMED_OUT;
    }

    private static int remainingTimeoutMs(long deadline) {
        long remainingNs = deadline - System.nanoTime();
        if (remainingNs <= 0L) {
            return 0;
        }
        return (int) Math.min(Integer.MAX_VALUE,
            (remainingNs + 999_999L) / 1_000_000L);
    }

    private static void runServer(RouterSocket server,
                                  AtomicBoolean stopped,
                                  AtomicReference<Throwable> failure) {
        try (Received received = new Received()) {
            while (true) {
                try {
                    server.recv(received, RecvFlags.NONE);
                } catch (ZlinkRecvException ex) {
                    if (PerfErrno.isRetryableRecv(ex.getNativeErrno())) {
                        continue;
                    }
                    throw ex;
                } catch (ZlinkException ex) {
                    if (PerfErrno.isRetryableRecv(ex.getNativeErrno())) {
                        continue;
                    }
                    throw ex;
                }
                if (received.parts().size() == 1
                    && PerfStopToken.isStopTokenMessage(received.firstPart())) {
                    stopped.set(true);
                    return;
                }
                if (received.replyToken().isEmpty()) {
                    received.close();
                    continue;
                }
                Message payload = PerfUtil.measurementPayload(received.parts());
                if (payload == null) {
                    received.close();
                    continue;
                }
                try (Message tail = PerfUtil.measurementPartCount() == 2
                         ? PerfUtil.measurementTail() : null) {
                    if (tail != null) {
                        received.reply().message(payload).message(tail).submit();
                    } else {
                        received.reply().message(payload).submit();
                    }
                }
                received.close();
            }
        } catch (Throwable ex) {
            failure.compareAndSet(null, ex);
        }
    }

    private static void sendStop(Socket client, boolean routedClient) {
        try (Message stop = PerfStopToken.newMessage()) {
            SendSubmission submission;
            if (routedClient) {
                submission = ((RouterSocket) client).send(SERVER_RID)
                    .message(stop).submit();
            } else {
                submission = ((DealerSocket) client).send().message(stop)
                    .submit();
            }
            SubmitResult result = submission.result();
            if (result == SubmitResult.BACKPRESSURED) {
                submission.admitted().toCompletableFuture().join();
            } else if (result != SubmitResult.OK) {
                throw new IllegalStateException(
                    "async stop send returned " + result);
            }
        }
    }
}
