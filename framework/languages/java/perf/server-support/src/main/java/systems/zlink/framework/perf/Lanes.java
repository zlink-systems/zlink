package systems.zlink.framework.perf;

import java.util.ArrayList;
import java.util.List;
import java.util.Optional;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import java.util.concurrent.atomic.AtomicInteger;
import java.util.function.IntFunction;

// §5 connect-concurrency for a source role's object preparation and probes: at most `concurrency` preparation calls are in
// progress; each finished one starts the next index. A failed item fails the whole preparation (the calls are not retried).
public final class Lanes {
    private Lanes() {}

    public static CompletionStage<Void> forEach(int count, int concurrency, IntFunction<CompletionStage<Void>> work) {
        AtomicInteger next = new AtomicInteger();
        List<CompletableFuture<Void>> lanes = new ArrayList<>();
        for (int lane = 0; lane < Math.min(concurrency, Math.max(1, count)); lane++) {
            CompletableFuture<Void> done = new CompletableFuture<>();
            lanes.add(done);
            CompletionLoop.run(done, () -> {
                int index = next.getAndIncrement();
                if (index >= count) {
                    return Optional.empty();
                }
                CompletionStage<Void> item = work.apply(index);
                return Optional.of(new CompletionLoop.Iteration<>(item, (ignored, error) -> {
                    if (error != null) {
                        done.completeExceptionally(error);
                    }
                }));
            });
        }
        return CompletableFuture.allOf(lanes.toArray(CompletableFuture[]::new));
    }
}
