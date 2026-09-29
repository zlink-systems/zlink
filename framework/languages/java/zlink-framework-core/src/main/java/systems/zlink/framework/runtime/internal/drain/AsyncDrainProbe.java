package systems.zlink.framework.runtime.internal.drain;

import java.util.ArrayList;
import java.util.List;
import java.util.Objects;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;

/** Assertion-only snapshot of named asynchronous work at a drain boundary. */
public final class AsyncDrainProbe {
    private final List<Work> work = new ArrayList<>();

    public void track(String name, String owner, CompletionStage<?> stage) {
        work.add(
                new Work(
                        Objects.requireNonNull(name, "name"),
                        Objects.requireNonNull(owner, "owner"),
                        Objects.requireNonNull(stage, "stage")));
    }

    public CompletableFuture<Void> expect(String name, String owner) {
        var terminal = new CompletableFuture<Void>();
        track(name, owner, terminal);
        return terminal;
    }

    public boolean completeOn(CompletionStage<?> stage, CompletableFuture<Void> obligation) {
        Objects.requireNonNull(stage, "stage")
                .whenComplete((ignored, failure) -> obligation.complete(null));
        return true;
    }

    public boolean completeOn(CompletionStage<?> stage, String name) {
        Work obligation = obligation(name);
        Objects.requireNonNull(stage, "stage")
                .whenComplete(
                        (ignored, failure) ->
                                obligation.stage().toCompletableFuture().complete(null));
        return true;
    }

    public boolean complete(String name) {
        obligation(name).stage().toCompletableFuture().complete(null);
        return true;
    }

    private Work obligation(String name) {
        return work.stream()
                .filter(item -> item.name().equals(name))
                .findFirst()
                .orElseThrow(() -> new AssertionError("unregistered drain work: " + name));
    }

    public List<Pending> pending() {
        return work.stream()
                .filter(item -> !item.stage().toCompletableFuture().isDone())
                .map(item -> new Pending(item.name(), item.owner()))
                .toList();
    }

    public List<Pending> registered() {
        return work.stream().map(item -> new Pending(item.name(), item.owner())).toList();
    }

    public void assertDrained() {
        List<Pending> incomplete = pending();
        if (!incomplete.isEmpty()) {
            throw new AssertionError("incomplete drain work: " + incomplete);
        }
    }

    public boolean assertDrainedResult() {
        assertDrained();
        return true;
    }

    /** Settles individually named work after its owner completes an explicit abort. */
    public boolean settleAbortedResult() {
        work.forEach(item -> item.stage().toCompletableFuture().complete(null));
        return assertDrainedResult();
    }

    public record Pending(String name, String owner) {}

    private record Work(String name, String owner, CompletionStage<?> stage) {}
}
