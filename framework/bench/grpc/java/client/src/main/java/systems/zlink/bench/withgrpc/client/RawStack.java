/* SPDX-License-Identifier: MPL-2.0 */

package systems.zlink.bench.withgrpc.client;

import java.nio.ByteBuffer;
import java.nio.charset.StandardCharsets;
import java.time.Duration;
import java.util.List;
import java.util.concurrent.ExecutionException;
import systems.zlink.bench.withgrpc.shared.BenchMetricHeader;
import systems.zlink.bench.withgrpc.shared.BenchTypedPayload;
import systems.zlink.bench.withgrpc.shared.RawWire;
import systems.zlink.contracts.core.Context;
import systems.zlink.contracts.core.RoutingId;
import systems.zlink.contracts.core.Zlink;
import systems.zlink.contracts.eventing.PollEventFlags;
import systems.zlink.contracts.eventing.PollEvents;
import systems.zlink.contracts.eventing.Poller;
import systems.zlink.contracts.messaging.Message;
import systems.zlink.contracts.messaging.SendSubmitOperation;
import systems.zlink.contracts.sockets.SubmitResult;
import systems.zlink.contracts.sockets.RouterSocket;
import systems.zlink.framework.runtime.internal.service.ZLinkFrameworkMultipartView;
import systems.zlink.framework.runtime.internal.service.ZLinkServiceM6AWireCodec;
import systems.zlink.framework.runtime.messaging.ZLinkJsonMessageSerializer;
import systems.zlink.framework.runtime.messaging.ZLinkMessagePayloads;
import systems.zlink.framework.runtime.messaging.ZLinkPayloadEncoding;
import systems.zlink.bench.withgrpc.shared.BenchContract;

/** Raw binding client: ROUTER&lt;-&gt;ROUTER with an explicit target routing ID. */
public final class RawStack implements AutoCloseable {
    private static final ZLinkServiceM6AWireCodec SERVICE_WIRE = new ZLinkServiceM6AWireCodec();
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
        SERVICE_WIRE.prepareChannelNames(List.of(BenchContract.CHANNEL_NAME));
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

    public RawOperation requestWithEnvelope(int kind, EnvelopeFactory envelopeFactory) {
        return new RawOperation() {
            @Override
            public RawSubmission submitRaw(int payloadSize, byte phase, long sequence) {
                var operation = router.request(peer);
                var body = ZLinkFrameworkMultipartView.owned(
                        RawWire.encodeBenchPayloadMessage(payloadSize, runId, phase, sequence));
                List<Message> envelope;
                try {
                    envelope = envelopeFactory.encode(kind, body);
                } catch (RuntimeException | Error failure) {
                    body.close();
                    throw failure;
                }
                try {
                    var submission = operation
                        .message(envelope.get(0))
                        .message(envelope.get(1))
                        .timeout(timeout)
                        .submit();
                    var admitted = submission.result() == SubmitResult.BACKPRESSURED
                        ? submission.admitted().toCompletableFuture()
                        : java.util.concurrent.CompletableFuture.<Void>completedFuture(
                            null);
                    return new RawSubmission(submission.result(),
                        admitted,
                        submission.reply().toCompletableFuture().thenAccept(
                            parts -> validateEnvelope(parts, runId, phase, payloadSize,
                                sequence)));
                } finally {
                    ZLinkFrameworkMultipartView.closeAll(envelope);
                }
            }
        };
    }

    public RawOperation requestWithServiceWireEnvelope(int kind, EnvelopeFactory envelopeFactory) {
        return requestWithServiceWireEnvelope(kind, envelopeFactory, false);
    }

    public RawOperation requestWithTypedJsonServiceWireEnvelope(
            int kind, EnvelopeFactory envelopeFactory) {
        return requestWithServiceWireEnvelope(kind, envelopeFactory, true);
    }

    private RawOperation requestWithServiceWireEnvelope(
            int kind, EnvelopeFactory envelopeFactory, boolean typedJson) {
        return new RawOperation() {
            @Override
            public RawSubmission submitRaw(int payloadSize, byte phase, long sequence) {
                var operation = router.request(peer);
                var body = payloadView(payloadSize, runId, phase, sequence, typedJson);
                List<Message> envelope;
                try {
                    envelope = envelopeFactory.encode(kind, body);
                } catch (RuntimeException | Error failure) {
                    body.close();
                    throw failure;
                }
                long correlation = sequence + 1;
                try (Message serviceHeader = Message.from(SERVICE_WIRE.encodeChannelRequestHeader(
                            correlation, BenchContract.CHANNEL_NAME, 0));
                        Message framed = ZLinkServiceM6AWireCodec.encodeFrameworkMultipartMessage(envelope)) {
                    var submission = operation.message(serviceHeader).message(framed)
                            .timeout(timeout).submit();
                    var admitted = submission.result() == SubmitResult.BACKPRESSURED
                        ? submission.admitted().toCompletableFuture()
                        : java.util.concurrent.CompletableFuture.<Void>completedFuture(null);
                    return new RawSubmission(submission.result(), admitted,
                        submission.reply().toCompletableFuture().thenAccept(parts -> {
                            try {
                                if (parts.size() != 2) {
                                    throw new IllegalArgumentException("invalid service reply frame count");
                                }
                                var reply = SERVICE_WIRE.decodeReplyHeader(parts.getFirst().dataBuffer());
                                if (reply.correlation() != correlation || reply.terminalResult() != 0) {
                                    throw new IllegalArgumentException("invalid service reply terminal");
                                }
                                try (var decoded = ZLinkServiceM6AWireCodec
                                        .decodeFrameworkMultipartFrameView(parts.get(1).dataBuffer())) {
                                    if (typedJson) {
                                        validateTypedEnvelope(
                                                decoded, runId, phase, payloadSize, sequence);
                                    } else {
                                        validateEnvelopeParts(
                                                decoded, runId, phase, payloadSize, sequence);
                                    }
                                }
                            } finally {
                                ZLinkFrameworkMultipartView.closeAll(parts);
                            }
                        }));
                } finally {
                    ZLinkFrameworkMultipartView.closeAll(envelope);
                }
            }
        };
    }

    public RawSendOperation send() {
        return new RawSendOperation();
    }

    public RawSendOperation sendWithEnvelope(int kind, EnvelopeFactory envelopeFactory) {
        return new FrameworkEnvelopeSendOperation(kind, envelopeFactory);
    }

    public RawSendOperation sendWithServiceWireEnvelope(int kind, EnvelopeFactory envelopeFactory) {
        return new ServiceWireEnvelopeSendOperation(kind, envelopeFactory, false);
    }

    public RawSendOperation sendWithTypedJsonServiceWireEnvelope(
            int kind, EnvelopeFactory envelopeFactory) {
        return new ServiceWireEnvelopeSendOperation(kind, envelopeFactory, true);
    }

    @FunctionalInterface
    public interface EnvelopeFactory {
        List<Message> encode(int kind, ZLinkFrameworkMultipartView body);
    }

    /** A send payload prepared by a logical stream before it enters the socket gate. */
    public static final class PreparedSend implements AutoCloseable {
        private final List<Message> submitParts;

        private PreparedSend(Message header, Message body) {
            this(List.of(header, body));
        }

        private PreparedSend(Message header, Message body, List<Message> ownedParts) {
            this(ownedParts == null ? List.of(header, body) : ownedParts);
        }

        private PreparedSend(List<Message> submitParts) {
            this.submitParts = submitParts;
        }

        @Override
        public void close() {
            ZLinkFrameworkMultipartView.closeAll(submitParts);
        }
    }

    public class RawSendOperation extends RawOperation {
        public PreparedSend prepareRaw(int payloadSize, byte phase, long sequence) {
            Message header = Message.from(RawWire.REQUEST_ENVELOPE);
            try {
                Message body = RawWire.encodeBenchPayloadMessage(
                    payloadSize, runId, phase, sequence);
                return new PreparedSend(header, body);
            } catch (RuntimeException | Error failure) {
                header.close();
                throw failure;
            }
        }

        /** Submits prepared messages and closes them after the binding snapshots them. */
        public RawSubmission submitRaw(PreparedSend prepared) {
            try {
                SendSubmitOperation operation =
                        router.send(peer).message(prepared.submitParts.getFirst());
                for (int index = 1; index < prepared.submitParts.size(); index++) {
                    operation = operation.message(prepared.submitParts.get(index));
                }
                var submission = operation.submit();
                var admitted = submission.result() == SubmitResult.BACKPRESSURED
                    ? submission.admitted().toCompletableFuture()
                    : java.util.concurrent.CompletableFuture.<Void>completedFuture(null);
                return new RawSubmission(
                    submission.result(),
                    admitted,
                    submission.result() == SubmitResult.OK
                        ? java.util.concurrent.CompletableFuture.completedFuture(null)
                        : admitted);
            } finally {
                prepared.close();
            }
        }

        @Override
        public RawSubmission submitRaw(int payloadSize, byte phase, long sequence) {
            return submitRaw(prepareRaw(payloadSize, phase, sequence));
        }
    }

    private final class FrameworkEnvelopeSendOperation extends RawSendOperation {
        private final int kind;
        private final EnvelopeFactory envelopeFactory;

        private FrameworkEnvelopeSendOperation(int kind, EnvelopeFactory envelopeFactory) {
            this.kind = kind;
            this.envelopeFactory = envelopeFactory;
        }

        @Override
        public PreparedSend prepareRaw(int payloadSize, byte phase, long sequence) {
            var body = ZLinkFrameworkMultipartView.owned(
                    RawWire.encodeBenchPayloadMessage(payloadSize, runId, phase, sequence));
            List<Message> parts;
            try {
                parts = envelopeFactory.encode(kind, body);
            } catch (RuntimeException | Error failure) {
                body.close();
                throw failure;
            }
            return new PreparedSend(parts.get(0), parts.get(1), parts);
        }
    }

    private final class ServiceWireEnvelopeSendOperation extends RawSendOperation {
        private final int kind;
        private final EnvelopeFactory envelopeFactory;
        private final boolean typedJson;

        private ServiceWireEnvelopeSendOperation(
                int kind, EnvelopeFactory envelopeFactory, boolean typedJson) {
            this.kind = kind;
            this.envelopeFactory = envelopeFactory;
            this.typedJson = typedJson;
        }

        @Override
        public PreparedSend prepareRaw(int payloadSize, byte phase, long sequence) {
            var body = payloadView(payloadSize, runId, phase, sequence, typedJson);
            List<Message> envelope;
            try {
                envelope = envelopeFactory.encode(kind, body);
            } catch (RuntimeException | Error failure) {
                body.close();
                throw failure;
            }
            try {
                return new PreparedSend(List.of(
                        Message.from(SERVICE_WIRE.encodeChannelSendHeader(
                                BenchContract.CHANNEL_NAME, 0)),
                        ZLinkServiceM6AWireCodec.encodeFrameworkMultipartMessage(envelope)));
            } finally {
                ZLinkFrameworkMultipartView.closeAll(envelope);
            }
        }
    }

    /** Raw operations expose admission separately from request completion. */
    public abstract class RawOperation implements BenchOperation {
        public abstract RawSubmission submitRaw(int payloadSize, byte phase, long sequence);

        /** Runs one pass of this socket's single-owner completion poller. */
        public void pollCompletions(Duration timeout) throws InterruptedException {
            completionPoller.wait(events, timeout);
        }

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

    private static void validateEnvelope(
        List<Message> parts, int runId, byte phase, int payloadSize, long sequence) {
        try {
            validateEnvelopeParts(parts, runId, phase, payloadSize, sequence);
        } finally {
            for (Message part : parts) {
                part.close();
            }
        }
    }

    private static void validateEnvelopeParts(
            List<Message> parts, int runId, byte phase, int payloadSize, long sequence) {
        var header = systems.zlink.framework.runtime.messaging.ZLinkChannelEnvelope
                .tryDecodeHeader(parts, false);
        if (header == null
                || header.kind()
                    != systems.zlink.framework.runtime.messaging.ZLinkChannelEnvelope.KIND_RESPONSE
                || !BenchContract.CHANNEL_NAME.equals(header.channelName())
                || !"BenchPayload".equals(header.messageName())
                || !"application/x-protobuf".equals(header.contentType())
                || parts.size() != 2) {
            throw new IllegalStateException("raw request returned an invalid reply envelope");
        }
        ByteBuffer body = RawWire.decodeBenchPayloadBody(parts.get(1).dataBuffer());
        BenchMetricHeader.Decoded decoded = BenchMetricHeader.decode(body);
        if (!BenchMetricHeader.isExpected(decoded, runId, phase, payloadSize, sequence)) {
            throw new IllegalStateException("raw reply header mismatch");
        }
    }

    private static void validateTypedEnvelope(
            List<Message> parts, int runId, byte phase, int payloadSize, long sequence) {
        var header = systems.zlink.framework.runtime.messaging.ZLinkChannelEnvelope
                .tryDecodeHeader(parts, false);
        if (header == null
                || header.kind()
                    != systems.zlink.framework.runtime.messaging.ZLinkChannelEnvelope.KIND_RESPONSE
                || !BenchContract.CHANNEL_NAME.equals(header.channelName())
                || !"BenchTypedPayload".equals(header.messageName())
                || !"application/json".equals(header.contentType())
                || parts.size() != 2) {
            throw new IllegalStateException("typed request returned an invalid reply envelope");
        }
        validateTyped(parts, runId, phase, payloadSize, sequence);
    }

    private static ZLinkFrameworkMultipartView payloadView(
            int payloadSize, int runId, byte phase, long sequence, boolean typedJson) {
        if (!typedJson) {
            return ZLinkFrameworkMultipartView.owned(
                    RawWire.encodeBenchPayloadMessage(payloadSize, runId, phase, sequence));
        }
        return ZLinkPayloadEncoding.encodeMessage(
                TypedJsonHolder.SERIALIZER,
                new BenchTypedPayload(
                        BenchMetricHeader.createPayload(payloadSize, runId, phase, sequence)),
                "application/json");
    }

    private static void validateTyped(
            List<Message> parts, int runId, byte phase, int payloadSize, long sequence) {
        if (parts.size() < 2) {
            throw new IllegalStateException("typed service reply returned too few parts");
        }
        var payload = ZLinkMessagePayloads.deserialize(
                TypedJsonHolder.SERIALIZER,
                parts.getLast().dataBuffer(),
                BenchTypedPayload.class);
        BenchMetricHeader.Decoded decoded = BenchMetricHeader.decode(payload.body());
        if (!BenchMetricHeader.isExpected(decoded, runId, phase, payloadSize, sequence)) {
            throw new IllegalStateException("typed JSON reply header mismatch");
        }
    }

    private static final class TypedJsonHolder {
        private static final ZLinkJsonMessageSerializer SERIALIZER =
                new ZLinkJsonMessageSerializer();
    }
    @Override
    public void close() {
        completionPoller.close();
        router.close();
    }
}
