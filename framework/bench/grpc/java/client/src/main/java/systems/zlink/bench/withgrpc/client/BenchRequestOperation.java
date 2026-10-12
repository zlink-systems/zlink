package systems.zlink.bench.withgrpc.client;

import java.util.concurrent.CompletableFuture;

/** Request stages submitted once and observed separately by the driver. */
@FunctionalInterface
public interface BenchRequestOperation extends BenchOperation {
    Submission submit(int payloadSize, byte phase, long sequence);

    @Override
    default CompletableFuture<Void> invoke(int payloadSize, byte phase, long sequence) {
        return submit(payloadSize, phase, sequence).reply();
    }

    record Submission(CompletableFuture<Void> admitted, CompletableFuture<Void> reply) {}
}
