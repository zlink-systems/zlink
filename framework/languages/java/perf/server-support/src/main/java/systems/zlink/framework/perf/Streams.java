package systems.zlink.framework.perf;

import java.util.ArrayList;
import java.util.List;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;

// §4.2 logical streams: `logicalStreams` independent closed loops, `inflight` operation chains each. This only starts the
// chains and joins them; each scenario's own step shows the public call and where its completion is recorded (§6.4).
public final class Streams {
    private Streams() {}

    /** One chain of a stream: it issues the stream's next operation until the window closes, then completes `done`. */
    @FunctionalInterface
    public interface Chain {
        void step(int stream, CompletableFuture<Void> done);
    }

    public static CompletionStage<Void> launch(RoleConfig config, Chain chain) {
        int streams = config.workload().logicalStreams();
        int inflight = config.workload().inflight();
        List<CompletableFuture<Void>> chains = new ArrayList<>(streams * inflight);
        for (int stream = 0; stream < streams; stream++) {
            for (int slot = 0; slot < inflight; slot++) {
                CompletableFuture<Void> done = new CompletableFuture<>();
                chains.add(done);
                chain.step(stream, done);
            }
        }
        return CompletableFuture.allOf(chains.toArray(CompletableFuture[]::new));
    }

}
