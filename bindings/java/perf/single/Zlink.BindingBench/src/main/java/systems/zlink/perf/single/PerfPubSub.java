/* SPDX-License-Identifier: MPL-2.0 */

package systems.zlink.perf.single;

import systems.zlink.contracts.core.Context;
import systems.zlink.contracts.messaging.Message;
import systems.zlink.contracts.eventing.MonitorEventType;
import systems.zlink.contracts.eventing.PollEventFlags;
import systems.zlink.contracts.sockets.PubSocket;
import systems.zlink.contracts.sockets.RecvFlags;
import systems.zlink.contracts.sockets.SendFlags;
import systems.zlink.contracts.sockets.SocketType;
import systems.zlink.contracts.sockets.SubSocket;
import systems.zlink.contracts.sockets.SubmitResult;
import systems.zlink.contracts.messaging.TopicMessage;
import systems.zlink.contracts.errors.ZlinkException;
import systems.zlink.contracts.errors.ZlinkSubmitException;
import systems.zlink.perf.PerfErrno;
import systems.zlink.perf.PerfSocketPollSet;
import systems.zlink.perf.PerfStopToken;
import systems.zlink.perf.PerfUtil;
import java.time.Duration;
import java.util.List;
import java.util.concurrent.atomic.AtomicReference;

final class PerfPubSub {
    private static final MonitorEventType READY_EVENT = MonitorEventType.CONNECTION_READY;
    private static final String TOPIC = "bench";

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
            PerfUtil.waitForMonitorEvent(subMonitor, READY_EVENT, 1,
                Duration.ofMillis(config.connectReadyTimeoutMs()),
                "pubsub subscriber ready");
            PerfUtil.waitForMonitorEvent(pubMonitor, READY_EVENT, 1,
                Duration.ofMillis(config.connectReadyTimeoutMs()),
                "pubsub publisher ready");
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
                // C parity: wait through the public poller, then drain every
                // currently available publication with DONT_WAIT. The loop
                // ends purely on the wire-level stop token.
                try (PerfSocketPollSet pollSet = PerfSocketPollSet.fromSockets(
                         List.of(sub), PollEventFlags.POLLIN);
                     TopicMessage received = new TopicMessage()) {
                    boolean stop = false;
                    while (!stop) {
                        pollSet.poll(-1);
                        while (true) {
                            if (!sub.subscribe(received,
                                    RecvFlags.DONT_WAIT)) {
                                break;
                            }
                            if (received.parts().size() == 1
                                && PerfStopToken.isStopTokenMessage(received.firstPart())) {
                                stop = true;
                                break;
                            }
                            Message payload = PerfUtil.measurementPayload(received.parts());
                            if (payload == null) {
                                continue;
                            }
                            long receivedNanoTime = System.nanoTime();
                            PerfUtil.Header header = PerfUtil.decodeHeader(
                                payload, config.size(),
                                receivedNanoTime);
                            if (header != null
                                && header.phase() == PerfUtil.PHASE_ACTIVE
                                && receivedNanoTime < activeEnd) {
                                metrics.recordNanos(header.latencyNanos());
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
                    if (!tryPublishBlocking(pub, active)) {
                        PerfUtil.pauseOneWaySendRetry("pubsub");
                    }
                }
            } finally {
                active.close();
            }
            // PERF_SINGLE_TEST_POLICY § 1.4: signal phase end with stop token
            // published on the same topic so the subscriber's filter delivers it.
            PerfStopToken.sendWithRetry(() -> {
                // C creates a fresh stop message for each non-blocking retry.
                // Do the same so a backpressured submit cannot leave a reused
                // message in an ambiguous native ownership state.
                try (Message stop = PerfStopToken.newMessage()) {
                    return tryPublish(pub, stop, SendFlags.DONT_WAIT, false);
                }
            }, "pubsub");
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

    private static boolean tryPublishBlocking(PubSocket pub, Message message) {
        return tryPublish(pub, message, SendFlags.NONE, true);
    }

    private static boolean tryPublish(PubSocket pub, Message message,
                                      SendFlags flags, boolean measurement) {
        try {
            if (measurement && PerfUtil.measurementPartCount() == 2) {
                pub.publish(TOPIC).message(message).message(PerfUtil.measurementTail())
                    .flags(flags).submit();
            } else {
                pub.publish(TOPIC).message(message).flags(flags).submit();
            }
            return true;
        } catch (ZlinkSubmitException ex) {
            if (ex.getResult() == SubmitResult.BACKPRESSURED) {
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
