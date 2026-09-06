/* SPDX-License-Identifier: MPL-2.0 */

package systems.zlink.bench.withgrpc.rawserver;

import java.nio.ByteBuffer;
import java.nio.charset.StandardCharsets;
import java.util.List;
import systems.zlink.bench.withgrpc.shared.Args;
import systems.zlink.bench.withgrpc.shared.BenchServerMetrics;
import systems.zlink.bench.withgrpc.shared.BenchStatsServer;
import systems.zlink.bench.withgrpc.shared.RawWire;
import systems.zlink.contracts.core.Context;
import systems.zlink.contracts.core.RoutingId;
import systems.zlink.contracts.core.Zlink;
import systems.zlink.contracts.messaging.Message;
import systems.zlink.contracts.messaging.Received;
import systems.zlink.contracts.sockets.RecvFlags;
import systems.zlink.contracts.sockets.RouterSocket;

/**
 * {@code zlink-<lang>} server, java row: raw binding, ROUTER&lt;-&gt;ROUTER
 * (FB-001, spec section 1.3).
 *
 * <p>Two ROUTERs, not one. spec section 3 separates the request echo endpoint from the
 * command endpoint so that an echo reply can never be counted in the
 * {@code send-saturation} receive total.
 *
 * <p>Each ROUTER has its own blocking receive thread, and the stats endpoint runs on
 * its own executor. Unlike the single-threaded node harness this server can block in
 * {@code recv} without stalling the settle poll (spec section 3 / FB-008).
 */
public final class ZLinkRawBenchServer {
    private ZLinkRawBenchServer() {
    }

    public static void main(String[] args) throws Exception {
        String endpoint = Args.value(args, "--endpoint", "tcp://127.0.0.1:5095");
        String commandEndpoint = Args.value(args, "--command-endpoint", "tcp://127.0.0.1:5097");
        String metricsUrl = Args.value(args, "--metrics-url", "http://127.0.0.1:5096");

        BenchServerMetrics metrics = new BenchServerMetrics();
        Context context = Zlink.createContext();
        RouterSocket requestRouter = context.createRouterSocket();
        RouterSocket commandRouter = context.createRouterSocket();

        requestRouter.setRoutingId(RoutingId.from(
            RawWire.RAW_REQUEST_SERVER_ID.getBytes(StandardCharsets.US_ASCII)));
        commandRouter.setRoutingId(RoutingId.from(
            RawWire.RAW_COMMAND_SERVER_ID.getBytes(StandardCharsets.US_ASCII)));
        requestRouter.options().mandatory(true);
        commandRouter.options().mandatory(true);
        requestRouter.bind(endpoint);
        commandRouter.bind(commandEndpoint);

        BenchStatsServer.start(metricsUrl, metrics,
            "{\"implementation\":\"zlink-java\",\"socket\":\"ROUTER<->ROUTER\","
            + "\"endpoint\":\"" + endpoint + "\",\"commandEndpoint\":\""
            + commandEndpoint + "\"}");

        Thread requestThread = new Thread(
            () -> pumpRequests(requestRouter, metrics), "bench-raw-request");
        Thread commandThread = new Thread(
            () -> pumpCommands(commandRouter, metrics), "bench-raw-command");
        requestThread.setDaemon(true);
        commandThread.setDaemon(true);
        requestThread.start();
        commandThread.start();

        System.err.println("[raw-server] request=" + endpoint + " command="
            + commandEndpoint + " stats=" + metricsUrl);
        Thread.currentThread().join();
    }

    private static final java.util.concurrent.atomic.AtomicInteger probeCount =
        new java.util.concurrent.atomic.AtomicInteger();

    private static void pumpRequests(RouterSocket socket, BenchServerMetrics metrics) {
        try (Received received = new Received()) {
            while (true) {
                try {
                    if (!socket.recv(received, RecvFlags.NONE)) {
                        continue;
                    }
                    ByteBuffer body = lastPartBody(received);
                    if (body == null) {
                        metrics.recordError();
                        received.close();
                        continue;
                    }
                    byte[] copy = new byte[body.remaining()];
                    body.get(copy);
                    byte[] reply = RawWire.encodeBenchPayload(copy);
                    // The two reply wrappers are closed after submit, as the .NET
                    // reference server does with `using` (ZLinkRawServer/Program.cs
                    // ReplyMultipart). A successful submit consumes them; an
                    // unsuccessful one leaves them caller-owned, and leaking a wrapper
                    // per reply is what starves the reply path at depth.
                    try (Message header = Message.from(RawWire.RESPONSE_ENVELOPE);
                         Message replyBody = Message.from(reply)) {
                        boolean hasToken = received.replyToken().isPresent();
                        if (System.getenv("BENCH_REPLY_PROBE") != null
                            && probeCount.getAndIncrement() < 5) {
                            System.err.println("[probe] reply branch hasToken=" + hasToken
                                + " parts=" + received.parts().size());
                        }
                        if (hasToken) {
                            received.reply().message(header).message(replyBody).submit();
                        } else {
                            received.send().message(header).message(replyBody).submit();
                        }
                    }
                } catch (Exception error) {
                    metrics.recordError();
                    System.err.println("raw request loop failed: " + error);
                }
                // The Received is NOT closed per iteration. The reply submit is
                // asynchronous and the reply token belongs to this Received, so closing
                // it here races the submit: measured against this server, a client
                // holding 4 requests outstanding got 1 reply and lost 3, while 2
                // outstanding still worked. The next recv resets the instance, which is
                // what the .NET reference server relies on (ZLinkRawServer/Program.cs).
            }
        }
    }

    private static void pumpCommands(RouterSocket socket, BenchServerMetrics metrics) {
        try (Received received = new Received()) {
            while (true) {
                try {
                    if (!socket.recv(received, RecvFlags.NONE)) {
                        continue;
                    }
                    ByteBuffer body = lastPartBody(received);
                    if (body == null) {
                        metrics.recordError();
                        continue;
                    }
                    // spec section 5 / G3: the send row's throughput is this count,
                    // taken on the server.
                    metrics.record(body);
                } catch (Exception error) {
                    metrics.recordError();
                    System.err.println("raw command loop failed: " + error);
                }
            }
        }
    }

    private static ByteBuffer lastPartBody(Received received) {
        List<Message> parts = received.parts();
        if (parts.isEmpty()) {
            return null;
        }
        return RawWire.decodeBenchPayloadBody(parts.get(parts.size() - 1).dataBuffer());
    }
}
