/* SPDX-License-Identifier: MPL-2.0 */

package systems.zlink.perf.single;

import systems.zlink.contracts.core.Context;
import systems.zlink.contracts.core.RoutingId;
import systems.zlink.contracts.messaging.Message;
import systems.zlink.contracts.messaging.Received;
import systems.zlink.contracts.sockets.DealerSocket;
import systems.zlink.contracts.sockets.RecvFlags;
import systems.zlink.contracts.sockets.RequestCallback;
import systems.zlink.contracts.sockets.RequestResult;
import systems.zlink.contracts.sockets.RouterSocket;
import systems.zlink.contracts.sockets.SendFlags;
import systems.zlink.contracts.sockets.Socket;
import systems.zlink.contracts.sockets.SubmitResult;
import systems.zlink.contracts.eventing.PollEventFlags;
import systems.zlink.contracts.errors.ZlinkException;
import systems.zlink.contracts.errors.ZlinkRecvException;
import systems.zlink.contracts.errors.ZlinkSubmitException;
import systems.zlink.perf.PerfStopToken;
import systems.zlink.perf.PerfErrno;
import systems.zlink.perf.PerfSocketPollSet;
import systems.zlink.perf.PerfUtil;
import systems.zlink.perf.PerfMessageTemplatePool;

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
        AtomicLong outstanding = new AtomicLong();
        try (Context ctx = PerfUtil.newContext(config);
             RouterSocket server = ctx.createRouterSocket();
             Socket client = routedClient ? ctx.createRouterSocket()
                                          : ctx.createDealerSocket();
             var serverMonitor = server.monitorOpen(
                 systems.zlink.contracts.eventing.MonitorEventType.CONNECTION_READY);
             var clientMonitor = client.monitorOpen(
                 systems.zlink.contracts.eventing.MonitorEventType.CONNECTION_READY)) {
            if (client instanceof RouterSocket router) {
                server.setRoutingId(SERVER_RID);
                server.options().mandatory(true);
                router.setRoutingId(CLIENT_RID);
                router.options().mandatory(true);
            } else {
                ((DealerSocket) client).setRoutingId(DEALER_RID);
            }
            PerfUtil.applyMonitorOptions(serverMonitor, config);
            PerfUtil.applyMonitorOptions(clientMonitor, config);
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
            int maxInFlight = Math.max(1,
                Math.min(64, 768 * 1024 / Math.max(1, config.size())));
            Duration requestTimeout = Duration.ofMillis(Math.max(1,
                PerfUtil.intEnv("PERF_SINGLE_REQREP_TIMEOUT_MS", 200)));
            RequestCallback callback = (result, parts) -> {
                try {
                    if (result == RequestResult.OK) {
                        if (parts != null && !parts.isEmpty()) {
                            PerfUtil.Header header = PerfUtil.decodeHeader(parts.get(0),
                                config.size());
                            if (header != null
                                && header.phase() == PerfUtil.PHASE_ACTIVE) {
                                metrics.recordNanos(header.latencyNanos());
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

            PerfSocketPollSet completions = PerfSocketPollSet.fromSockets(
                List.of(client), PollEventFlags.POLLCOMPLETION);
            try (PerfMessageTemplatePool requestPool =
                     new PerfMessageTemplatePool(config.size(), maxInFlight)) {
                while (System.nanoTime() < activeEnd && failure.get() == null) {
                    boolean submittedAny = false;
                    int submittedSinceProgress = 0;
                    while (outstanding.get() < maxInFlight
                        && System.nanoTime() < activeEnd
                        && failure.get() == null) {
                        Message request = requestPool.acquire(config.size(),
                            (byte) PerfUtil.PHASE_ACTIVE, System.nanoTime(), activeEnd);
                        if (request == null) {
                            break;
                        }
                        try (request) {
                            outstanding.incrementAndGet();
                            boolean accepted = submit(client, routedClient, request,
                                requestTimeout, callback);
                            if (!accepted) {
                                outstanding.decrementAndGet();
                                break;
                            }
                            submittedAny = true;
                            submittedSinceProgress++;
                            if (submittedSinceProgress >= 64) {
                                submittedSinceProgress = 0;
                                completions.poll(0);
                            }
                        } catch (ZlinkSubmitException ex) {
                            outstanding.decrementAndGet();
                            if (ex.getResult() == SubmitResult.BACKPRESSURED
                                || ex.getResult() == SubmitResult.NOT_CONNECTED) {
                                break;
                            }
                            throw ex;
                        }
                    }
                    if (!submittedAny && outstanding.get() == 0) {
                        pauseIdleRequester();
                        continue;
                    }
                    completions.poll(50);
                }
                long drainEnd = System.nanoTime()
                    + TimeUnit.MILLISECONDS.toNanos(completionDrainTimeoutMs);
                while (outstanding.get() > 0 && System.nanoTime() < drainEnd) {
                    completions.poll(50);
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
            } finally {
                completions.close();
            }
        }
    }

    private static void pauseIdleRequester() {
        try {
            Thread.sleep(1L);
        } catch (InterruptedException ex) {
            Thread.currentThread().interrupt();
            throw new IllegalStateException("requester idle wait interrupted", ex);
        }
    }

    private static boolean submit(Socket client, boolean routedClient,
                                  Message request, Duration timeout,
                                  RequestCallback callback) {
        if (routedClient) {
            return ((RouterSocket) client).request(SERVER_RID)
                .message(request).timeout(timeout).flags(SendFlags.NONE)
                .submit(callback);
        }
        return ((DealerSocket) client).request()
            .message(request).timeout(timeout).flags(SendFlags.NONE)
            .submit(callback);
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
                if (PerfStopToken.isStopTokenMessage(received.firstPart())) {
                    stopped.set(true);
                    return;
                }
                if (received.requestSeq().isEmpty()) {
                    received.close();
                    continue;
                }
                Message reply = Message.from(received.firstPart());
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
                received.reply().message(reply).submit();
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
        try (PerfSocketPollSet writable = PerfSocketPollSet.fromSockets(
                 List.of(client), PollEventFlags.POLLOUT)) {
            writable.setEvents(0);
            PerfStopToken.sendWithRetry(
                () -> trySendStop(client, routedClient),
                () -> {
                    writable.setEvents(0, PollEventFlags.POLLOUT);
                    writable.poll(-1);
                },
                "socket reqrep");
        }
    }

    private static boolean trySendStop(Socket client, boolean routedClient) {
        try (Message stop = PerfStopToken.newMessage()) {
            if (routedClient) {
                return ((RouterSocket) client).send(SERVER_RID)
                    .message(stop).flags(SendFlags.DONT_WAIT).submit();
            }
            return ((DealerSocket) client).send()
                .message(stop).flags(SendFlags.DONT_WAIT).submit();
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
