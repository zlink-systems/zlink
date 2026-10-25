/* SPDX-License-Identifier: MPL-2.0 */
package systems.zlink.bench.withgrpc.client;

import io.grpc.ServerBuilder;
import io.grpc.stub.StreamObserver;
import java.io.ByteArrayInputStream;
import java.io.IOException;
import java.io.InputStream;
import java.net.ServerSocket;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicInteger;
import systems.zlink.bench.withgrpc.proto.BenchPayload;
import systems.zlink.bench.withgrpc.proto.BenchServiceGrpc;

/** Piped stdin releases a real source's transport and both HTTP listeners, including input failure. */
public final class SourceLifecycleTest {
    public static void main(String[] args) throws Exception {
        AtomicInteger probes = new AtomicInteger();
        var target = ServerBuilder.forPort(0).addService(new BenchServiceGrpc.BenchServiceImplBase() {
            @Override public void echo(BenchPayload request, StreamObserver<BenchPayload> observer) {
                probes.incrementAndGet();
                observer.onNext(request);
                observer.onCompleted();
            }
        }).build().start();
        InputStream original = System.in;
        try {
            runSource(target.getPort(), new ByteArrayInputStream(new byte[] {'\n'}), false);
            runSource(target.getPort(), new InputStream() {
                @Override public int read() throws IOException { throw new IOException("expected input failure"); }
            }, true);
            if (probes.get() != 2) throw new AssertionError("source did not open both real transports");
        } finally {
            System.setIn(original);
            target.shutdown();
            if (!target.awaitTermination(5, TimeUnit.SECONDS)) throw new AssertionError("target did not close");
        }
        System.out.println("SOURCE_LIFECYCLE_OK");
    }

    private static void runSource(int targetPort, InputStream input, boolean fail) throws Exception {
        int triggerPort = freePort();
        int statsPort = freePort();
        System.setIn(input);
        try {
            BenchClient.main(new String[] {
                "--implementation", "grpc-java", "--scenario", "request-serial", "--run-id", "source-lifecycle",
                "--trigger-url", "http://127.0.0.1:" + triggerPort, "--stats-url", "http://127.0.0.1:" + statsPort,
                "--target-stats-url", "http://127.0.0.1:1", "--target-endpoint", "127.0.0.1:" + targetPort
            });
            if (fail) throw new AssertionError("input failure was swallowed");
        } catch (IOException expected) {
            if (!fail || !expected.getMessage().equals("expected input failure")) throw expected;
        }
        try (ServerSocket trigger = new ServerSocket(triggerPort);
             ServerSocket stats = new ServerSocket(statsPort)) {
            if (!trigger.isBound() || !stats.isBound()) throw new AssertionError("source HTTP listeners leaked");
        }
    }

    private static int freePort() throws IOException {
        try (ServerSocket socket = new ServerSocket(0)) { return socket.getLocalPort(); }
    }
}
