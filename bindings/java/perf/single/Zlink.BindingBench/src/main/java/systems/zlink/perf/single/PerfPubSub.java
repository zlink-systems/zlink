/* SPDX-License-Identifier: MPL-2.0 */

package systems.zlink.perf.single;

import systems.zlink.contracts.core.Context;
import systems.zlink.contracts.messaging.Message;
import systems.zlink.contracts.eventing.MonitorEventType;
import systems.zlink.contracts.sockets.PubSocket;
import systems.zlink.contracts.sockets.RecvFlags;
import systems.zlink.contracts.sockets.SendFlags;
import systems.zlink.contracts.sockets.SocketType;
import systems.zlink.contracts.sockets.SubSocket;
import systems.zlink.contracts.messaging.TopicMessage;
import systems.zlink.perf.PerfStopToken;
import systems.zlink.perf.PerfUtil;
import java.time.Duration;
import java.util.concurrent.atomic.AtomicReference;

final class PerfPubSub {
    private static final MonitorEventType READY_EVENT = MonitorEventType.CONNECTION_READY;
    private static final String TOPIC = "perf.topic";

    private PerfPubSub() {
    }

    static PerfUtil.Result run(PerfUtil.Config config) {
        String endpoint = PerfUtil.endpoint(config.transport(), "single-pubsub");
        AtomicReference<Throwable> failure = new AtomicReference<>();
        PerfUtil.Metrics metrics = new PerfUtil.Metrics(config);
        // C parity: perf_pubsub.cpp run_pubsub (~185-192) creates ONE
        // ctx_guard_t and builds BOTH the publisher and subscriber sockets on
        // it. Using two independent contexts for non-inproc transports (the
        // prior behaviour) doubled the io-thread scheduling for the PUB->SUB
        // pipe and capped subscriber throughput ~4x vs C. Share one context
        // for all transports to match C.
        Context ctx = PerfUtil.newContext(config);
        try (PubSocket pub = ctx.createPubSocket();
             SubSocket sub = ctx.createSubSocket()) {
            var pubMonitor = pub.monitorOpen(MonitorEventType.CONNECTION_READY);
            var subMonitor = sub.monitorOpen(MonitorEventType.CONNECTION_READY);
            try {
            PerfUtil.applyMonitorOptions(pubMonitor, config);
            PerfUtil.applyMonitorOptions(subMonitor, config);
            PerfUtil.applySocketOptions(pub, config);
            PerfUtil.applySocketOptions(sub, config);
            // C parity: perf_pubsub.cpp resolve_pubsub_xpub_nodrop_opt
            // (~18-22) and apply (~198-200): env PERF_SINGLE_PUBSUB_XPUB_NODROP
            // defaults to enabled; only the exact value "0" disables NODROP.
            // C always sets ZLINK_PUB_OPT_NODROP explicitly to the resolved
            // 0/1, so mirror that here.
            pub.options().noDrop(resolvePubSubXpubNoDrop());
            // C parity: PerfUtil.newContext applies the benchmark message size
            // as the context auto-HWM message unit before sockets are created.
            // Recalculating here keeps that per-message sizing in effect when
            // the transport pipe is created.
            PerfUtil.recalculateAutoHwm(ctx);
            PerfUtil.configureServerTls(pub, config.transport());
            PerfUtil.configureClientTls(sub, config.transport());
            pub.bind(PerfUtil.bindEndpoint(endpoint, config.transport()));
            // C parity: perf_pubsub.cpp setup_connected_pubsub_pair (~37)
            // subscribes to "" (subscribe-all). Subscribing to a specific
            // topic instead forces the core SUB side to run a topic-filter
            // match on every delivered message, which C avoids entirely and
            // which throttled steady-state subscriber throughput ~4x.
            sub.setSubscription("");
            sub.connect(PerfUtil.connectedEndpoint(pub, endpoint,
                config.transport()));
            PerfUtil.waitForMonitorEvent(pubMonitor, READY_EVENT, 1,
                Duration.ofMillis(config.connectReadyTimeoutMs()),
                "pubsub publisher ready");
            PerfUtil.waitForMonitorEvent(subMonitor, READY_EVENT, 1,
                Duration.ofMillis(config.connectReadyTimeoutMs()),
                "pubsub subscriber ready");
            PerfUtil.printSingleMonitorAutoHwm(config, pubMonitor, "publisher",
                SocketType.PUB);
            PerfUtil.printSingleMonitorAutoHwm(config, subMonitor, "subscriber",
                SocketType.SUB);
            } finally {
                pubMonitor.close();
                subMonitor.close();
            }
            settleAfterReady();

            // PERF_SINGLE_TEST_POLICY § 1.4: receiver waits with -1 and exits
            // on wire-level stop token published on TOPIC.
            long activeEnd = System.nanoTime()
                + config.durationSeconds() * 1_000_000_000L;
            Thread recvThread = new Thread(() -> {
                // C parity: perf_single_one_way.hpp run_active_phase receiver
                // thread (~276-327). C does NOT poll the SUB socket: it issues
                // a blocking subscribe (bounded by RCVTIMEO so an idle socket
                // returns EAGAIN and the loop keeps cycling) and, on each
                // payload, burst-drains with ZLINK_DONTWAIT until EAGAIN.
                // Using a poller here added a poll(-1) syscall round trip per
                // ~200-message batch which, combined with PUB NODROP
                // backpressure, throttled steady-state throughput ~4x. The
                // loop ends purely on the wire-level stop token.
                try (TopicMessage received = new TopicMessage()) {
                    boolean stop = false;
                    while (!stop) {
                        if (!sub.subscribe(received, RecvFlags.NONE)) {
                            // RCVTIMEO elapsed with no data: keep cycling
                            // (matches C recv_result_again -> continue).
                            continue;
                        }
                        if (PerfStopToken.isStopTokenMessage(
                                received.firstPart())) {
                            return;
                        }
                        PerfUtil.Header header = PerfUtil.decodeHeader(
                            received.firstPart(), config.size());
                        if (header != null
                            && header.phase() == PerfUtil.PHASE_ACTIVE
                            && System.nanoTime() < activeEnd) {
                            metrics.recordNanos(header.latencyNanos());
                        }
                        // Inner DONT_WAIT burst drain (C lines ~289-315).
                        while (true) {
                            if (!sub.subscribe(received,
                                    RecvFlags.DONT_WAIT)) {
                                break;
                            }
                            if (PerfStopToken.isStopTokenMessage(
                                    received.firstPart())) {
                                stop = true;
                                break;
                            }
                            PerfUtil.Header burst = PerfUtil.decodeHeader(
                                received.firstPart(), config.size());
                            if (burst != null
                                && burst.phase() == PerfUtil.PHASE_ACTIVE
                                && System.nanoTime() < activeEnd) {
                                metrics.recordNanos(burst.latencyNanos());
                            }
                        }
                    }
                } catch (Throwable ex) {
                    failure.compareAndSet(null, ex);
                }
            }, "single-pubsub-recv");
            recvThread.start();
            Message active = PerfUtil.payloadTemplate(config.size());
            try {
                while (System.nanoTime() < activeEnd) {
                    active = PerfUtil.resetAndWritePayload(active, config.size(),
                        (byte) PerfUtil.PHASE_ACTIVE, System.nanoTime());
                    if (!publishWithRetry(pub, active, activeEnd)) {
                        break;
                    }
                }
            } finally {
                active.close();
            }
            // PERF_SINGLE_TEST_POLICY § 1.4: signal phase end with stop token
            // published on the same topic so the subscriber's filter delivers it.
            try (Message stop = PerfStopToken.newMessage()) {
                long stopDeadline = System.nanoTime() + 2_000_000_000L;
                publishWithRetry(pub, stop, stopDeadline);
            }
            PerfUtil.join(recvThread, "pubsub receiver",
                Duration.ofSeconds(config.durationSeconds() + 10L));
            if (failure.get() != null) {
                throw new IllegalStateException("pubsub receiver failed", failure.get());
            }
            return metrics.finishSingle(config);
        } finally {
            ctx.close();
        }
    }

    // C parity: perf_pubsub.cpp resolve_pubsub_xpub_nodrop_opt (~18-22):
    //   (env && strcmp(env,"0")==0) ? 0 : 1
    // Default enabled; only the exact string "0" disables it. Any other
    // value (including unset/blank) leaves NODROP enabled.
    private static boolean resolvePubSubXpubNoDrop() {
        String env = System.getenv("PERF_SINGLE_PUBSUB_XPUB_NODROP");
        return env == null || !env.equals("0");
    }

    private static void settleAfterReady() {
        int settleMs = PerfUtil.intEnv("PERF_SINGLE_PUBSUB_READY_SETTLE_MS", 1000);
        if (settleMs <= 0) {
            return;
        }
        try {
            Thread.sleep(settleMs);
        } catch (InterruptedException ex) {
            Thread.currentThread().interrupt();
            throw new IllegalStateException("pubsub settle interrupted", ex);
        }
    }

    private static boolean publishWithRetry(PubSocket pub, Message message,
                                            long deadlineNs) {
        // C parity (perf_pubsub.cpp send step + perf_single_one_way.hpp
        // send_active_samples): nonblocking publish; on transient backpressure
        // retry immediately (send_step_retry -> continue) until the deadline,
        // with no POLLOUT wait or sleep.
        while (System.nanoTime() < deadlineNs) {
            if (pub.publish(TOPIC)
                    .message(message)
                    .flags(SendFlags.DONT_WAIT)
                    .submit()) {
                return true;
            }
        }
        return false;
    }
}
