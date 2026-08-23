/* SPDX-License-Identifier: MPL-2.0 */

package systems.zlink.perf.multi;

import systems.zlink.contracts.core.Context;
import systems.zlink.contracts.messaging.Message;
import systems.zlink.contracts.eventing.MonitorEventType;
import systems.zlink.contracts.eventing.SocketMonitor;
import systems.zlink.contracts.eventing.PollEventFlags;
import systems.zlink.contracts.sockets.PubSocket;
import systems.zlink.contracts.sockets.RecvFlags;
import systems.zlink.contracts.sockets.SendFlags;
import systems.zlink.contracts.sockets.Socket;
import systems.zlink.contracts.sockets.SocketType;
import systems.zlink.contracts.sockets.SubSocket;
import systems.zlink.contracts.errors.ZlinkSubmitException;
import systems.zlink.contracts.sockets.SubmitResult;
import systems.zlink.contracts.messaging.TopicMessage;
import systems.zlink.perf.PerfControl;
import systems.zlink.perf.PerfSocketPollSet;
import systems.zlink.perf.PerfStopToken;
import systems.zlink.perf.PerfUtil;
import java.time.Duration;
import java.util.ArrayList;
import java.util.Collections;
import java.util.List;

final class PerfMultiPubSub {
    private static final String TOPIC = "bench";
    private static final MonitorEventType READY_EVENT =
        MonitorEventType.CONNECTION_READY;
    private static final int RECEIVE_POLL_TIMEOUT_MS = 100;

    private PerfMultiPubSub() {
    }

    static PerfUtil.Result runServer(PerfUtil.Config config) {
        try (Context ctx = PerfUtil.newContext(config);
             PubSocket pub = ctx.createPubSocket()) {
            PerfUtil.applySocketOptions(pub, config);
            PerfUtil.configureServerTls(pub, config.transport());
            pub.bind(config.endpoint());
            PerfControl.emitReady(config.endpoint());
            PerfControl.awaitStart(config.size(), "pubsub server");
            // C parity: PerfUtil.newContext applies the benchmark message size
            // as the context auto-HWM message unit. Recalculate AFTER the start
            // signal, once all SUB clients have connected, so the PUB's
            // per-connection fan-out send queues are resized.
            PerfUtil.recalculateAutoHwm(ctx);
            PerfUtil.printMultiSocketAutoHwm(config, pub, "server",
                "server", SocketType.PUB);
            long activeEnd = System.nanoTime() + config.durationSeconds() * 1_000_000_000L;
            Message active = PerfUtil.payloadTemplate(config.size());
            try (PerfSocketPollSet writable = PerfSocketPollSet.fromSockets(
                    List.of(pub), PollEventFlags.POLLOUT)) {
                writable.setEvents(0);
                while (System.nanoTime() < activeEnd) {
                    active = PerfUtil.resetAndWritePayload(active, config.size(),
                        (byte) PerfUtil.PHASE_ACTIVE, System.nanoTime());
                    publishWithBackpressureWait(pub, writable, active);
                }
            } finally {
                active.close();
            }
            // C parity (perf_multi_pubsub_server.cpp publish_stop_token): after
            // the active window publish a wire-level stop token on the topic so
            // the subscriber wakes from its signal-driven (-1) poll and exits.
            publishStopToken(pub);
            return PerfUtil.Result.silent(config);
        }
    }

    static PerfUtil.Result runClient(PerfUtil.Config config) {
        PerfUtil.Metrics metrics = new PerfUtil.Metrics(config);
        List<SubSocket> subscribers = new ArrayList<>(config.clients());
        List<SocketMonitor> monitors = new ArrayList<>(config.clients());
        List<TopicMessage> receivedSlots = new ArrayList<>(config.clients());
        Context ctx = PerfUtil.newContext(config);
        try {
            for (int i = 0; i < config.clients(); i++) {
                SubSocket sub = ctx.createSubSocket();
                SocketMonitor monitor = sub.monitorOpen(READY_EVENT);
                PerfUtil.applyMonitorOptions(monitor, config);
                PerfUtil.applySocketOptions(sub, config);
                PerfUtil.configureClientTls(sub, config.transport());
                // C parity: perf_multi_client_helpers.hpp subscribes SUB
                // clients to "" (k_subscribe_all). A specific-topic
                // subscription makes the core SUB side run a trie/filter
                // match per delivered message on every one of the 100
                // fan-out connections.
                sub.setSubscription("");
                subscribers.add(sub);
                monitors.add(monitor);
                receivedSlots.add(new TopicMessage());
            }
            // C parity: perf_multi_client_helpers.hpp applies
            // apply_benchmark_socket_options (auto-HWM) BEFORE zlink_connect
            // (~308 vs ~380). Recalculate the context auto-HWM here, before
            // any connect, so the per-message sizing is in effect when each
            // SUB transport pipe is created. Connecting first and recalcing
            // afterwards left tcp SUB pipes on the pre-recalc default and
            // collapsed MULTI_PUBSUB/tcp ~10x while tls/ws/wss were far less
            // affected.
            PerfUtil.recalculateAutoHwm(ctx);
            for (SubSocket sub : subscribers) {
                sub.connect(config.endpoint());
            }
            // C parity: refresh_connected_client_auto_hwm (~394) re-applies
            // after connect.
            PerfUtil.recalculateAutoHwm(ctx);
            for (int i = 0; i < monitors.size(); i++) {
                PerfUtil.printMultiMonitorAutoHwm(config, monitors.get(i),
                    "client", "client[" + i + "]", SocketType.SUB);
            }
            Duration readyTimeout = Duration.ofMillis(config.connectReadyTimeoutMs());
            for (int i = 0; i < monitors.size(); i++) {
                PerfUtil.waitForMonitorEvent(monitors.get(i), READY_EVENT, 1,
                    readyTimeout, "pubsub client ready[" + i + "]");
                monitors.get(i).close();
            }
            monitors.clear();
            PerfControl.emitClientReady(config.size());
            PerfControl.awaitStart(config.size(), "pubsub client");

            List<Socket> pollSockets = new ArrayList<>(subscribers.size());
            pollSockets.addAll(subscribers);
            try (PerfSocketPollSet pollSet = PerfSocketPollSet.fromSockets(
                pollSockets, PollEventFlags.POLLIN)) {
                long activeEnd = System.nanoTime()
                    + config.durationSeconds() * 1_000_000_000L;
                // Match C run_recv_duration: the active deadline ends the
                // receive loop. A delayed stop token must not extend the
                // measurement or drain an already queued PUB/SUB backlog.
                boolean phaseDone = false;
                while (!phaseDone) {
                    long remainingNs = activeEnd - System.nanoTime();
                    if (remainingNs <= 0L) {
                        break;
                    }
                    int waitMs = (int) Math.min(RECEIVE_POLL_TIMEOUT_MS,
                        Math.max(1L, remainingNs / 1_000_000L));
                    int readyCount = pollSet.poll(waitMs);
                    if (readyCount <= 0) {
                        continue;
                    }
                    for (int readyOffset = 0; readyOffset < readyCount; readyOffset++) {
                        int index = pollSet.readyIndexAt(readyOffset);
                        if (!pollSet.readyHasEventAt(readyOffset,
                            PollEventFlags.POLLIN)) {
                            continue;
                        }
                        if (drainSubscriber(subscribers.get(index),
                            receivedSlots.get(index), config, metrics,
                            activeEnd)) {
                            phaseDone = true;
                        }
                    }
                }
            }
            return metrics.finishMulti(config);
        } finally {
            for (TopicMessage received : receivedSlots) {
                try {
                    received.close();
                } catch (RuntimeException ignored) {
                }
            }
            for (SocketMonitor monitor : monitors) {
                try {
                    monitor.close();
                } catch (RuntimeException ignored) {
                }
            }
            for (SubSocket sub : subscribers) {
                try {
                    sub.disconnect(config.endpoint());
                } catch (RuntimeException ignored) {
                }
                try {
                    sub.close();
                } catch (RuntimeException ignored) {
                }
            }
            // This benchmark process exits after one size case. Closing the
            // native context here can wait behind one-way PUB/SUB teardown and
            // hide the already-finished RESULT from the runner.
        }
    }

    private static void publishWithBackpressureWait(PubSocket pub,
                                                    PerfSocketPollSet writable,
                                                    Message message) {
        while (true) {
            try {
                pub.publish(TOPIC)
                    .message(message)
                    .flags(SendFlags.DONT_WAIT)
                    .submit();
                return;
            } catch (ZlinkSubmitException ex) {
                if (!isTransientSubmit(ex)) {
                    throw ex;
                }
            }
            writable.setEvents(0, PollEventFlags.POLLOUT);
            writable.poll(-1);
            writable.setEvents(0);
        }
    }

    private static boolean isTransientSubmit(ZlinkSubmitException ex) {
        return ex.getResult() == SubmitResult.BACKPRESSURED
            || ex.getResult() == SubmitResult.NOT_CONNECTED;
    }

    // Returns true when the wire-level stop token (or cooldown phase) is seen,
    // mirroring C perf_multi_pubsub_client.cpp recv_one_pubsub_message: the
    // stop token is checked before header decode and ends the phase; counting
    // remains bounded by the active deadline.
    private static boolean drainSubscriber(SubSocket sub, TopicMessage received,
                                           PerfUtil.Config config,
                                           PerfUtil.Metrics metrics,
                                           long activeEnd) {
        while (true) {
            // C checks the active deadline while draining each ready socket.
            // Do the same so one busy SUB cannot consume the post-deadline
            // interval before the outer loop observes it.
            if (System.nanoTime() >= activeEnd) {
                return true;
            }
            if (!sub.subscribe(received, RecvFlags.DONT_WAIT)) {
                return false;
            }
            if (PerfStopToken.isStopTokenMessage(received.firstPart())) {
                return true;
            }
            int phase = PerfUtil.recordOneWayLatency(metrics,
                received.firstPart(), config.size(), activeEnd);
            if (phase == PerfUtil.PHASE_UNKNOWN) {
                continue;
            }
            if (phase == PerfUtil.PHASE_COOLDOWN) {
                return true;
            }
        }
    }

    // C retries a transient stop-token publish until it succeeds. The token
    // terminates the subscriber phase, so reporting a successful server run
    // without delivering it changes the benchmark lifecycle.
    private static void publishStopToken(PubSocket pub) {
        while (true) {
            try (Message stop = PerfStopToken.newMessage()) {
                pub.publish(TOPIC)
                    .message(stop)
                    .flags(SendFlags.NONE)
                    .submit();
                return;
            } catch (ZlinkSubmitException ex) {
                if (!isTransientSubmit(ex)) {
                    throw ex;
                }
            }
        }
    }
}
