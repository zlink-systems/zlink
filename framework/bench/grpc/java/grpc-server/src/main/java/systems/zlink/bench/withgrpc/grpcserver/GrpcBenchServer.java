/* SPDX-License-Identifier: MPL-2.0 */

package systems.zlink.bench.withgrpc.grpcserver;

import com.google.protobuf.Empty;
import io.grpc.Server;
import io.grpc.ServerBuilder;
import io.grpc.stub.StreamObserver;
import systems.zlink.bench.withgrpc.proto.BenchPayload;
import systems.zlink.bench.withgrpc.proto.BenchServiceGrpc;
import systems.zlink.bench.withgrpc.shared.Args;
import systems.zlink.bench.withgrpc.shared.BenchServerMetrics;
import systems.zlink.bench.withgrpc.shared.BenchStatsServer;

/**
 * {@code grpc-<lang>} server, java row. spec section 8.1: grpc-java with
 * {@code grpc-netty-shaded}, in the library's default server configuration --
 * spec section 8.2 fixes that gRPC is left at each language's default and the
 * configuration is recorded rather than tuned.
 */
public final class GrpcBenchServer {
    private GrpcBenchServer() {
    }

    public static void main(String[] args) throws Exception {
        int port = Args.integer(args, "--port", 5091);
        String metricsUrl = Args.value(args, "--metrics-url", "http://127.0.0.1:5094");

        BenchServerMetrics metrics = new BenchServerMetrics();
        Server server = ServerBuilder.forPort(port)
            .addService(new BenchService(metrics))
            .build()
            .start();

        BenchStatsServer.start(metricsUrl, metrics,
            "{\"implementation\":\"grpc-java\",\"serverConfiguration\":"
            + "\"io.grpc.ServerBuilder.forPort default configuration, plaintext loopback,"
            + " grpc-netty-shaded 1.72.0\"}");
        System.err.println("[grpc-server] listening on 127.0.0.1:" + port
            + " stats=" + metricsUrl);
        Runtime.getRuntime().addShutdownHook(new Thread(server::shutdownNow));
        server.awaitTermination();
    }

    private static final class BenchService extends BenchServiceGrpc.BenchServiceImplBase {
        private final BenchServerMetrics metrics;

        private BenchService(BenchServerMetrics metrics) {
            this.metrics = metrics;
        }

        // spec section 2: Echo returns the payload, so the client can validate the
        // 29-byte header that came back (G2).
        @Override
        public void echo(BenchPayload request, StreamObserver<BenchPayload> observer) {
            observer.onNext(request);
            observer.onCompleted();
        }

        // FB-002: the send comparison keeps this unary Command returning Empty. No
        // client-streaming RPC is added and no RPC is added to the proto.
        @Override
        public void command(BenchPayload request, StreamObserver<Empty> observer) {
            metrics.record(request.getBody().asReadOnlyByteBuffer());
            observer.onNext(Empty.getDefaultInstance());
            observer.onCompleted();
        }
    }
}
