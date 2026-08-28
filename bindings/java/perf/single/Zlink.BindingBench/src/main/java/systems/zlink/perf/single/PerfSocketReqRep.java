/* SPDX-License-Identifier: MPL-2.0 */

package systems.zlink.perf.single;

import systems.zlink.contracts.core.Context;
import systems.zlink.contracts.core.RoutingId;
import systems.zlink.contracts.messaging.Message;
import systems.zlink.contracts.messaging.Received;
import systems.zlink.contracts.eventing.PollEventFlags;
import systems.zlink.contracts.sockets.DealerSocket;
import systems.zlink.contracts.sockets.RecvFlags;
import systems.zlink.contracts.sockets.RequestResult;
import systems.zlink.contracts.sockets.RouterSocket;
import systems.zlink.contracts.sockets.Socket;
import systems.zlink.contracts.sockets.SubmitResult;
import systems.zlink.contracts.sockets.SendFlags;
import systems.zlink.contracts.errors.ZlinkException;
import systems.zlink.contracts.errors.ZlinkRecvException;
import systems.zlink.contracts.errors.ZlinkSubmitException;
import systems.zlink.perf.PerfStopToken;
import systems.zlink.perf.PerfSocketPollSet;
import systems.zlink.perf.PerfErrno;
import systems.zlink.perf.PerfUtil;

import java.nio.charset.StandardCharsets;
import java.time.Duration;
import java.util.List;
import java.util.concurrent.TimeUnit;
import java.util.function.BiConsumer;
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
        AtomicLong outstanding = new AtomicLong();
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
            Thread serverThread = new Thread(() -> runServer(server, serverStopped,
                failure, completionDrainTimeoutMs),
                "single-socket-reqrep-server");
            serverThread.start();

            long activeEnd = System.nanoTime()
                + config.durationSeconds() * 1_000_000_000L;
            Duration requestTimeout = Duration.ofMillis(Math.max(1,
                PerfUtil.intEnv("PERF_SINGLE_REQREP_TIMEOUT_MS", 200)));
            BiConsumer<RequestResult, List<Message>> completion =
                (result, parts) -> {
                try {
                    long completedAt = System.nanoTime();
                    if (result == RequestResult.OK) {
                        if (completedAt < activeEnd) {
                            Message payload = PerfUtil.measurementPayload(parts);
                            if (payload != null) {
                                PerfUtil.Header header = PerfUtil.decodeHeader(
                                    payload, config.size(), completedAt);
                                if (header != null
                                    && header.phase() == PerfUtil.PHASE_ACTIVE) {
                                    metrics.recordNanos(header.latencyNanos());
                                }
                            }
                        }
                    } else if (result != RequestResult.TIMED_OUT) {
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

            try (PerfSocketPollSet completionPoller =
                     PerfSocketPollSet.fromSockets(List.of(client),
                         PollEventFlags.POLLCOMPLETION)) {
                while (System.nanoTime() < activeEnd && failure.get() == null) {
                    boolean submittedAny = false;
                    while (System.nanoTime() < activeEnd
                        && failure.get() == null) {
                        try (Message request = PerfUtil.payload(config.size(),
                                 (byte) PerfUtil.PHASE_ACTIVE,
                                 System.nanoTime())) {
                            outstanding.incrementAndGet();
                            submit(client, routedClient, request,
                                requestTimeout, completion);
                            submittedAny = true;
                        } catch (ZlinkSubmitException ex) {
                            outstanding.decrementAndGet();
                            if (ex.getResult() == SubmitResult.BACKPRESSURED
                                || ex.getResult() == SubmitResult.NOT_CONNECTED) {
                                break;
                            }
                            throw ex;
                        }
                    }
                    if (!submittedAny && System.nanoTime() < activeEnd) {
                        completionPoller.poll(remainingTimeoutMs(activeEnd));
                    }
                }
                long drainEnd = System.nanoTime()
                    + TimeUnit.MILLISECONDS.toNanos(completionDrainTimeoutMs);
                while (outstanding.get() > 0 && System.nanoTime() < drainEnd) {
                    completionPoller.poll(remainingTimeoutMs(drainEnd));
                }
                sendStop(client, routedClient);
                PerfUtil.join(serverThread, "socket reqrep server",
                    Duration.ofSeconds(10));
                if (!serverStopped.get() || outstanding.get() != 0
                    || failure.get() != null) {
                    throw new IllegalStateException("socket reqrep failed",
                        failure.get());
                }
                return metrics.finishSingle(config);
            }
        }
    }

    private static void submit(Socket client, boolean routedClient,
                               Message request, Duration timeout,
                               BiConsumer<RequestResult, List<Message>> completion) {
        if (routedClient) {
            if (PerfUtil.measurementPartCount() == 2) {
                ((RouterSocket) client).request(SERVER_RID).message(request)
                    .message(PerfUtil.measurementTail()).timeout(timeout)
                    .submit_sync(SendFlags.DONT_WAIT, completion);
            } else {
                ((RouterSocket) client).request(SERVER_RID).message(request)
                    .timeout(timeout)
                    .submit_sync(SendFlags.DONT_WAIT, completion);
            }
        } else if (PerfUtil.measurementPartCount() == 2) {
            ((DealerSocket) client).request().message(request)
                .message(PerfUtil.measurementTail()).timeout(timeout)
                .submit_sync(SendFlags.DONT_WAIT, completion);
        } else {
            ((DealerSocket) client).request().message(request).timeout(timeout)
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

    private static void runServer(RouterSocket server,
                                  AtomicBoolean stopped,
                                  AtomicReference<Throwable> failure,
                                  int completionDrainTimeoutMs) {
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
                if (received.requestSeq().isEmpty()) {
                    received.close();
                    continue;
                }
                Message payload = PerfUtil.measurementPayload(received.parts());
                if (payload == null) {
                    received.close();
                    continue;
                }
                Message reply = Message.from(payload);
                submitReplyWithRetry(received, reply, completionDrainTimeoutMs);
                received.close();
            }
        } catch (Throwable ex) {
            failure.compareAndSet(null, ex);
        }
    }

    private static void submitReplyWithRetry(Received received, Message reply,
                                             int timeoutMs) {
        long retryEnd = System.nanoTime()
            + TimeUnit.MILLISECONDS.toNanos(timeoutMs);
        while (true) {
            try {
                if (PerfUtil.measurementPartCount() == 2) {
                    received.reply().message(reply).message(PerfUtil.measurementTail()).submit();
                } else {
                    received.reply().message(reply).submit();
                }
                return;
            } catch (ZlinkSubmitException ex) {
                if (ex.getResult() != SubmitResult.BACKPRESSURED
                    || System.nanoTime() >= retryEnd) {
                    reply.close();
                    throw ex;
                }
                Thread.yield();
            }
        }
    }

    private static void sendStop(Socket client, boolean routedClient) {
        PerfStopToken.sendWithRetry(
            () -> trySendStop(client, routedClient),
            "socket reqrep");
    }

    private static boolean trySendStop(Socket client, boolean routedClient) {
        try (Message stop = PerfStopToken.newMessage()) {
            if (routedClient) {
                ((RouterSocket) client).send(SERVER_RID).message(stop)
                    .submit_sync(SendFlags.NONE);
                return true;
            }
            ((DealerSocket) client).send().message(stop)
                .submit_sync(SendFlags.NONE);
            return true;
        } catch (ZlinkSubmitException ex) {
            if (ex.getResult() == SubmitResult.BACKPRESSURED
                || ex.getResult() == SubmitResult.NOT_CONNECTED) {
                return false;
            }
            throw ex;
        } catch (ZlinkException ex) {
            if (PerfErrno.isRetryableSend(ex.getNativeErrno())) {
                return false;
            }
            throw ex;
        }
    }
}
