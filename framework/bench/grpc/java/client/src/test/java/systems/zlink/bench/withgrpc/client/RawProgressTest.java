/* SPDX-License-Identifier: MPL-2.0 */

package systems.zlink.bench.withgrpc.client;

import java.nio.charset.StandardCharsets;
import java.time.Duration;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicBoolean;
import systems.zlink.bench.withgrpc.shared.BenchHttpApplication;
import systems.zlink.bench.withgrpc.shared.BenchMetricHeader;
import systems.zlink.bench.withgrpc.shared.RawWire;
import systems.zlink.contracts.core.Context;
import systems.zlink.contracts.core.RoutingId;
import systems.zlink.contracts.core.Zlink;
import systems.zlink.contracts.eventing.PollEventFlags;
import systems.zlink.contracts.eventing.PollEvents;
import systems.zlink.contracts.eventing.Poller;
import systems.zlink.contracts.messaging.Message;
import systems.zlink.contracts.messaging.Received;
import systems.zlink.contracts.sockets.RecvFlags;
import systems.zlink.contracts.sockets.RouterSocket;

/** Real socket coverage for uncapped progress and phase-local poller ownership. */
public final class RawProgressTest {
    public static void main(String[] args) throws Exception {
        BenchOptions options = new BenchOptions(new String[] {
            "--scenario", "request-backpressure", "--implementation", "zlink-java",
            "--run-id", "raw-progress-test", "--cell-id", "raw-progress-test",
            "--trigger-url", "http://127.0.0.1:1", "--stats-url", "http://127.0.0.1:2",
            "--target-stats-url", "http://127.0.0.1:3"
        });
        String endpoint = "inproc://raw-progress-test";
        String peer = "raw-progress-server";
        AtomicBoolean running = new AtomicBoolean(true);
        CompletableFuture<Void> serverDone = new CompletableFuture<>();
        try (Context context = Zlink.createContext();
             RouterSocket server = context.createRouterSocket()) {
            server.setRoutingId(RoutingId.from(peer.getBytes(StandardCharsets.US_ASCII)));
            server.options().mandatory(true);
            server.bind(endpoint);
            Thread.ofPlatform().name("raw-progress-test-server").start(() -> {
                try {
                    serve(server, running);
                    serverDone.complete(null);
                } catch (Exception error) {
                    serverDone.completeExceptionally(error);
                }
            });
            try (RawStack stack = RawStack.create(context, options,
                    "raw-progress-client", peer, endpoint)) {
                BenchDrivers drivers = new BenchDrivers(options);
                RawStack.RawOperation request = stack.request();
                drivers.waitForRouteReady(request, 1024);
                BenchHttpApplication.Trigger trigger = new BenchHttpApplication.Trigger(
                    options.runIdText, options.cellId, options.scenario, 1024,
                    "warmup", 200, options.requestWindow, options.sendConcurrency);
                drivers.runWarmup(trigger, request);
                BenchHttpApplication.Counters first = drivers.counters();
                check(first.peakInFlight() >= 2,
                    "the next request must be submitted while the first reply is held");
                checkDrained(first);

                // Closing the phase poller must return completion ownership to
                // the runtime, so the ordinary serial invocation also completes.
                request.invoke(1024, BenchMetricHeader.PHASE_WARMUP, 100_000)
                    .get(2, TimeUnit.SECONDS);
                drivers.runWarmup(trigger, request);
                checkDrained(drivers.counters());
            } finally {
                running.set(false);
                serverDone.get(2, TimeUnit.SECONDS);
            }
        }
        System.out.println("JAVA_RAW_PROGRESS_OK");
    }

    private static void serve(RouterSocket server, AtomicBoolean running) {
        try (Poller poller = Zlink.createPoller();
             Received held = new Received();
             Received current = new Received()) {
            poller.add(server, 0, PollEventFlags.POLLIN);
            PollEvents events = new PollEvents(1);
            int count = 0;
            while (running.get()) {
                if (poller.wait(events, Duration.ofMillis(100)) == 0) {
                    continue;
                }
                Received received = count == 1 ? held : current;
                if (!server.recv(received, RecvFlags.DONT_WAIT)) {
                    continue;
                }
                count++;
                // Echo the route-ready probe. Hold the first measured request
                // until another arrives: a serial or window=1 loop cannot pass.
                if (count == 2) {
                    continue;
                }
                if (count == 3) {
                    reply(held);
                }
                reply(received);
            }
        }
    }

    private static void reply(Received received) {
        try (Message header = Message.from(RawWire.RESPONSE_ENVELOPE);
             Message body = received.parts().getLast().copy()) {
            received.reply().message(header).message(body).submit();
        }
    }

    private static void checkDrained(BenchHttpApplication.Counters counters) {
        check(counters.submitted() > 1, "no requests submitted");
        check(counters.errors() == 0, "request errors: " + counters.errors());
        check(counters.inFlight() == 0, "requests abandoned: " + counters.inFlight());
        check(counters.submitted() == counters.completed(), "reply count mismatch");
    }

    private static void check(boolean condition, String message) {
        if (!condition) {
            throw new AssertionError(message);
        }
    }
}
