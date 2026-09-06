/* SPDX-License-Identifier: MPL-2.0 */

package systems.zlink.bench.withgrpc.client;

import java.nio.ByteBuffer;
import java.nio.charset.StandardCharsets;
import java.time.Duration;
import java.util.List;
import java.util.concurrent.CompletableFuture;
import systems.zlink.bench.withgrpc.shared.BenchMetricHeader;
import systems.zlink.bench.withgrpc.shared.RawWire;
import systems.zlink.contracts.core.Context;
import systems.zlink.contracts.core.RoutingId;
import systems.zlink.contracts.messaging.Message;
import systems.zlink.contracts.sockets.DealerSocket;
import systems.zlink.contracts.sockets.RouterSocket;
import systems.zlink.contracts.sockets.Socket;

/**
 * {@code zlink-java} client: raw binding, ROUTER&lt;-&gt;ROUTER by default.
 *
 * <p>FB-001 / spec section 1.3: the raw row is ROUTER&lt;-&gt;ROUTER so that
 * {@code zlink-framework-java / zlink-java} isolates framework-layer cost instead of
 * mixing in a DEALER-&gt;ROUTER socket-pattern difference. The DEALER mode exists only
 * for the one comparison run the campaign keeps beside the three ROUTER runs.
 */
public final class RawStack implements AutoCloseable {
    private final Socket socket;
    private final RouterSocket router;
    private final DealerSocket dealer;
    private final RoutingId peer;
    private final int runId;
    private final Duration timeout;

    private RawStack(
        Socket socket, RouterSocket router, DealerSocket dealer, RoutingId peer,
        int runId, Duration timeout) {
        this.socket = socket;
        this.router = router;
        this.dealer = dealer;
        this.peer = peer;
        this.runId = runId;
        this.timeout = timeout;
    }

    public static RawStack create(
        Context context, BenchOptions options, String selfId, String peerId, String endpoint) {
        RoutingId peer = RoutingId.from(peerId.getBytes(StandardCharsets.US_ASCII));
        RoutingId self = RoutingId.from(selfId.getBytes(StandardCharsets.US_ASCII));
        Duration timeout = Duration.ofMillis(options.requestTimeoutMs);
        if ("dealer".equals(options.rawSocket)) {
            DealerSocket dealer = context.createDealerSocket();
            dealer.setRoutingId(self);
            dealer.connect(endpoint);
            return new RawStack(dealer, null, dealer, peer, options.runId, timeout);
        }
        RouterSocket router = context.createRouterSocket();
        router.setRoutingId(self);
        router.options().mandatory(true);
        router.options().setConnectRoutingId(peer);
        router.connect(endpoint);
        return new RawStack(router, router, null, peer, options.runId, timeout);
    }

    public BenchOperation request() {
        return (payloadSize, phase, sequence) -> {
            byte[] payload =
                BenchMetricHeader.createPayload(payloadSize, runId, phase, sequence);
            var operation = router != null ? router.request(peer) : dealer.request();
            return operation
                .message(Message.from(RawWire.REQUEST_ENVELOPE))
                .message(Message.from(RawWire.encodeBenchPayload(payload)))
                .timeout(timeout)
                .submit()
                .toCompletableFuture()
                .thenAccept(parts -> validate(parts, runId, phase, payloadSize, sequence));
        };
    }

    public BenchOperation send() {
        return (payloadSize, phase, sequence) -> {
            byte[] payload =
                BenchMetricHeader.createPayload(payloadSize, runId, phase, sequence);
            var operation = router != null ? router.send(peer) : dealer.send();
            return operation
                .message(Message.from(RawWire.REQUEST_ENVELOPE))
                .message(Message.from(RawWire.encodeBenchPayload(payload)))
                .submit()
                .toCompletableFuture()
                .thenApply(ignored -> (Void) null);
        };
    }

    private static void validate(
        List<Message> parts, int runId, byte phase, int payloadSize, long sequence) {
        try {
            if (parts.isEmpty()) {
                throw new IllegalStateException("raw request returned no reply parts");
            }
            ByteBuffer body = RawWire.decodeBenchPayloadBody(
                parts.get(parts.size() - 1).dataBuffer());
            BenchMetricHeader.Decoded decoded = BenchMetricHeader.decode(body);
            if (!BenchMetricHeader.isExpected(decoded, runId, phase, payloadSize, sequence)) {
                throw new IllegalStateException("raw reply header mismatch");
            }
        } finally {
            for (Message part : parts) {
                part.close();
            }
        }
    }

    @Override
    public void close() {
        socket.close();
    }
}
