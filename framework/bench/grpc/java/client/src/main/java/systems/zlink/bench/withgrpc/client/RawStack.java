/* SPDX-License-Identifier: MPL-2.0 */

package systems.zlink.bench.withgrpc.client;

import java.nio.ByteBuffer;
import java.nio.charset.StandardCharsets;
import java.time.Duration;
import java.util.List;
import systems.zlink.bench.withgrpc.shared.BenchMetricHeader;
import systems.zlink.bench.withgrpc.shared.RawWire;
import systems.zlink.contracts.core.Context;
import systems.zlink.contracts.core.RoutingId;
import systems.zlink.contracts.messaging.Message;
import systems.zlink.contracts.sockets.RouterSocket;

/** Raw binding client: ROUTER&lt;-&gt;ROUTER with an explicit target routing ID. */
public final class RawStack implements AutoCloseable {
    private final RouterSocket router;
    private final RoutingId peer;
    private final int runId;
    private final Duration timeout;

    private RawStack(
        RouterSocket router, RoutingId peer,
        int runId, Duration timeout) {
        this.router = router;
        this.peer = peer;
        this.runId = runId;
        this.timeout = timeout;
    }

    public static RawStack create(
        Context context, BenchOptions options, String selfId, String peerId, String endpoint) {
        RoutingId peer = RoutingId.from(peerId.getBytes(StandardCharsets.US_ASCII));
        RoutingId self = RoutingId.from(selfId.getBytes(StandardCharsets.US_ASCII));
        Duration timeout = Duration.ofMillis(options.requestTimeoutMs);
        RouterSocket router = context.createRouterSocket();
        router.setRoutingId(self);
        router.options().mandatory(true);
        router.options().setConnectRoutingId(peer);
        router.connect(endpoint);
        return new RawStack(router, peer, options.runId, timeout);
    }

    public BenchOperation request() {
        return (payloadSize, phase, sequence) -> {
            var operation = router.request(peer);
            try (Message header = Message.from(RawWire.REQUEST_ENVELOPE);
                 Message body = RawWire.encodeBenchPayloadMessage(
                     payloadSize, runId, phase, sequence)) {
                return operation
                    .message(header)
                    .message(body)
                    .timeout(timeout)
                    .submit()
                    .reply()
                    .toCompletableFuture()
                    .thenAccept(parts -> validate(parts, runId, phase, payloadSize, sequence));
            }
        };
    }

    public BenchOperation send() {
        return (payloadSize, phase, sequence) -> {
            var operation = router.send(peer);
            try (Message header = Message.from(RawWire.REQUEST_ENVELOPE);
                 Message body = RawWire.encodeBenchPayloadMessage(
                     payloadSize, runId, phase, sequence)) {
                return operation
                    .message(header)
                    .message(body)
                    .submit()
                    .admitted()
                    .toCompletableFuture()
                    .thenApply(ignored -> (Void) null);
            }
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
        router.close();
    }
}
