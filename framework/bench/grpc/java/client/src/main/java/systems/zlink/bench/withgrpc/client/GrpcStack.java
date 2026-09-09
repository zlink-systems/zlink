/* SPDX-License-Identifier: MPL-2.0 */

package systems.zlink.bench.withgrpc.client;

import com.google.common.util.concurrent.FutureCallback;
import com.google.common.util.concurrent.Futures;
import com.google.common.util.concurrent.ListenableFuture;
import com.google.common.util.concurrent.MoreExecutors;
import com.google.protobuf.ByteString;
import io.grpc.ManagedChannel;
import io.grpc.ManagedChannelBuilder;
import java.util.concurrent.CompletableFuture;
import systems.zlink.bench.withgrpc.proto.BenchPayload;
import systems.zlink.bench.withgrpc.proto.BenchServiceGrpc;
import systems.zlink.bench.withgrpc.shared.BenchMetricHeader;

/** {@code grpc-java} client: grpc-java unary RPC, default channel configuration. */
public final class GrpcStack implements AutoCloseable {
    private final ManagedChannel channel;
    private final BenchServiceGrpc.BenchServiceFutureStub stub;
    private final int runId;

    public GrpcStack(BenchOptions options) {
        this.channel = ManagedChannelBuilder.forTarget(options.grpcUrl)
            .usePlaintext()
            .build();
        this.stub = BenchServiceGrpc.newFutureStub(channel);
        this.runId = options.runId;
    }

    public BenchOperation echo() {
        return (payloadSize, phase, sequence) -> {
            byte[] body = BenchMetricHeader.createPayload(payloadSize, runId, phase, sequence);
            BenchPayload request = BenchPayload.newBuilder()
                .setBody(ByteString.copyFrom(body))
                .build();
            CompletableFuture<Void> result = new CompletableFuture<>();
            addCallback(stub.echo(request), result, reply -> {
                // G2: the reply's 29-byte header is validated, not assumed.
                BenchMetricHeader.Decoded decoded =
                    BenchMetricHeader.decode(reply.getBody().asReadOnlyByteBuffer());
                if (!BenchMetricHeader.isExpected(decoded, runId, phase, payloadSize, sequence)) {
                    throw new IllegalStateException("grpc echo reply header mismatch");
                }
            });
            return result;
        };
    }

    public BenchOperation command() {
        return (payloadSize, phase, sequence) -> {
            byte[] body = BenchMetricHeader.createPayload(payloadSize, runId, phase, sequence);
            BenchPayload request = BenchPayload.newBuilder()
                .setBody(ByteString.copyFrom(body))
                .build();
            CompletableFuture<Void> result = new CompletableFuture<>();
            // FB-002: the send comparison uses this unary Command returning Empty. gRPC
            // has no one-way primitive, so it pays a round trip for a command that needs
            // no reply; that cost belongs in a service-side comparison and is not hidden.
            addCallback(stub.command(request), result, reply -> { });
            return result;
        };
    }

    private static <T> void addCallback(
        ListenableFuture<T> future, CompletableFuture<Void> result, Validator<T> validator) {
        Futures.addCallback(future, new FutureCallback<T>() {
            @Override
            public void onSuccess(T value) {
                try {
                    validator.validate(value);
                    result.complete(null);
                } catch (RuntimeException error) {
                    result.completeExceptionally(error);
                }
            }

            @Override
            public void onFailure(Throwable error) {
                result.completeExceptionally(error);
            }
        }, MoreExecutors.directExecutor());
    }

    @FunctionalInterface
    private interface Validator<T> {
        void validate(T value);
    }

    @Override
    public void close() {
        channel.shutdownNow();
    }
}
