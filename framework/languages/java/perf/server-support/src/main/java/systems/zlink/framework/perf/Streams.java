package systems.zlink.framework.perf;

import java.util.ArrayList;
import java.util.List;
import java.util.Optional;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import java.util.function.IntFunction;

// §4.2–§4.3: stream count and the next-call rule are shared here; each scenario supplies the public
// call and its result
// recording (§6.4).
public final class Streams {
    private Streams() {}

    /**
     * One chain of a stream: it issues the stream's next operation until the window closes, then
     * completes `done`.
     */
    @FunctionalInterface
    public interface Chain {
        void step(int stream, CompletableFuture<Void> done);
    }

    public static CompletionStage<Void> launch(
            RoleConfig config, Measurement measurement, Chain chain) {
        int streams = config.workload().logicalStreams();
        List<CompletableFuture<Void>> chains = new ArrayList<>(streams);
        for (int stream = 0; stream < streams; stream++) {
            CompletableFuture<Void> done = new CompletableFuture<>();
            chains.add(done);
            chain.step(stream, done);
        }
        return CompletableFuture.allOf(chains.toArray(CompletableFuture[]::new))
                .thenCompose(ignored -> measurement.operationsDrained());
    }

    /**
     * Request calls advance on submission, not on reply; the completion loop drains all submitted
     * replies.
     */
    public static <T> CompletionStage<Void> launchRequests(
            RoleConfig config,
            Measurement measurement,
            IntFunction<Optional<CompletionLoop.Iteration<T>>> next) {
        CompletableFuture<Void> done = new CompletableFuture<>();
        CompletionLoop.runRequests(done, config.workload().logicalStreams(), next);
        return done.thenCompose(ignored -> measurement.operationsDrained());
    }
}
