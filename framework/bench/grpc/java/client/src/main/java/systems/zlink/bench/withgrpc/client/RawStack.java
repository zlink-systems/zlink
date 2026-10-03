/* SPDX-License-Identifier: MPL-2.0 */

package systems.zlink.bench.withgrpc.client;

import java.nio.ByteBuffer;
import java.nio.charset.StandardCharsets;
import java.time.Duration;
import java.util.List;
import java.util.concurrent.ExecutionException;
import systems.zlink.bench.withgrpc.shared.BenchMetricHeader;
import systems.zlink.bench.withgrpc.shared.RawWire;
import systems.zlink.contracts.core.Context;
import systems.zlink.contracts.core.RoutingId;
import systems.zlink.contracts.core.Zlink;
import systems.zlink.contracts.eventing.PollEventFlags;
import systems.zlink.contracts.eventing.PollEvents;
import systems.zlink.contracts.eventing.Poller;
import systems.zlink.contracts.messaging.Message;
import systems.zlink.contracts.sockets.SubmitResult;
import systems.zlink.contracts.sockets.RouterSocket;

/** Raw binding client: ROUTER&lt;-&gt;ROUTER with an explicit target routing ID. */
public final class RawStack implements AutoCloseable {
    private final RouterSocket router;
    private final Poller completionPoller;
    private final PollEvents events = new PollEvents(1);
    private final RoutingId peer;
    private final int runId;
    private final Duration timeout;

    private RawStack(
        RouterSocket router, Poller completionPoller, RoutingId peer,
        int runId, Duration timeout) {
        this.router = router;
        this.completionPoller = completionPoller;
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
        Poller completionPoller = null;
        try {
            completionPoller = Zlink.createPoller();
            completionPoller.add(router, 0, PollEventFlags.POLLCOMPLETION);
            return new RawStack(router, completionPoller, peer, options.runId, timeout);
        } catch (RuntimeException | Error error) {
            if (completionPoller != null) {
                completionPoller.close();
            }
            router.close();
            throw error;
        }
    }

    public RawOperation request() {
        return new RawOperation() {
            @Override
            public RawSubmission submitRaw(int payloadSize, byte phase, long sequence) {
                var operation = router.request(peer);
                try (Message header = Message.from(RawWire.REQUEST_ENVELOPE);
                     Message body = RawWire.encodeBenchPayloadMessage(
                         payloadSize, runId, phase, sequence)) {
                    var submission = operation
                        .message(header)
                        .message(body)
                        .timeout(timeout)
                        .submit();
                    var admitted = submission.result() == SubmitResult.BACKPRESSURED
                        ? submission.admitted().toCompletableFuture()
                        : java.util.concurrent.CompletableFuture.<Void>completedFuture(
                            null);
                    return new RawSubmission(submission.result(),
                        admitted,
                        submission.reply().toCompletableFuture().thenAccept(
                            parts -> validate(parts, runId, phase, payloadSize,
                                sequence)));
                }
            }
        };
    }

    public RawOperation send() {
        return new RawOperation() {
            @Override
            public RawSubmission submitRaw(int payloadSize, byte phase, long sequence) {
                var operation = router.send(peer);
                try (Message header = Message.from(RawWire.REQUEST_ENVELOPE);
                     Message body = RawWire.encodeBenchPayloadMessage(
                         payloadSize, runId, phase, sequence)) {
                    var submission = operation
                        .message(header)
                        .message(body)
                        .submit();
                    var admitted = submission.result() == SubmitResult.BACKPRESSURED
                        ? submission.admitted().toCompletableFuture()
                        : java.util.concurrent.CompletableFuture.<Void>completedFuture(
                            null);
                    return new RawSubmission(submission.result(), admitted,
                        submission.result() == SubmitResult.OK
                            ? java.util.concurrent.CompletableFuture.completedFuture(
                                null)
                            : admitted);
                }
            }
        };
    }

    /** Raw operations expose admission separately from request completion. */
    public abstract class RawOperation implements BenchOperation {
        public abstract RawSubmission submitRaw(int payloadSize, byte phase, long sequence);

        /** Drives this socket's completion poller: one non-blocking pass, then waits for the rest. */
        @Override
        public boolean await(
                java.util.concurrent.CompletableFuture<Void> completion, Duration timeout)
                throws InterruptedException, ExecutionException {
            long deadline = BenchMetricHeader.nowNs() + timeout.toNanos();
            completionPoller.wait(events, Duration.ZERO);
            while (!completion.isDone()) {
                long remaining = deadline - BenchMetricHeader.nowNs();
                if (remaining <= 0L) {
                    return false;
                }
                completionPoller.wait(events, Duration.ofNanos(remaining));
            }
            completion.get();
            return true;
        }

        @Override
        public final java.util.concurrent.CompletableFuture<Void> invoke(
                int payloadSize, byte phase, long sequence) {
            return submitRaw(payloadSize, phase, sequence).completion();
        }
    }

    /** Initial result, conditional admission wait, and independent completion. */
    public record RawSubmission(
        SubmitResult result,
        java.util.concurrent.CompletableFuture<Void> admitted,
        java.util.concurrent.CompletableFuture<Void> completion) {
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
        completionPoller.close();
        router.close();
    }
}
