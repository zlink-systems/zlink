package systems.zlink.framework.runtime.internal.drain;

import java.util.ArrayList;
import java.util.List;
import java.util.Objects;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;

/** Assertion-only record of named work at an owner drain boundary. */
public final class AsyncDrainProbe {
    private final List<Work> work = new ArrayList<>();

    public CompletableFuture<Void> expect(String name, String owner) {
        var obligation = new CompletableFuture<Void>();
        work.add(new Work(Objects.requireNonNull(name), Objects.requireNonNull(owner), obligation));
        return obligation;
    }

    public boolean completeOn(CompletionStage<?> stage, String name) {
        return completeOn(stage, obligation(name).terminal());
    }

    public boolean completeOn(CompletionStage<?> stage, CompletableFuture<Void> obligation) {
        Objects.requireNonNull(stage)
                .whenComplete(
                        (ignored, failure) -> Objects.requireNonNull(obligation).complete(null));
        return true;
    }

    public boolean complete(String name) {
        obligation(name).terminal().complete(null);
        return true;
    }

    public boolean assertDrainedResult() {
        assertDrained();
        return true;
    }

    public <T> CompletionStage<T> assertDrainedOnSuccess(CompletionStage<T> stage) {
        return stage.whenComplete(
                (ignored, failure) -> {
                    if (failure == null) {
                        assertDrained();
                    }
                });
    }

    public void assertDrained() {
        List<Pending> pending = pending();
        if (!pending.isEmpty()) {
            throw new AssertionError("incomplete drain work: " + pending);
        }
    }

    public List<Pending> pending() {
        return work.stream()
                .filter(item -> !item.terminal().isDone())
                .map(item -> new Pending(item.name(), item.owner()))
                .toList();
    }

    private Work obligation(String name) {
        return work.stream()
                .filter(item -> item.name().equals(name))
                .findFirst()
                .orElseThrow(() -> new AssertionError("unregistered drain work: " + name));
    }

    public record Pending(String name, String owner) {}

    private record Work(String name, String owner, CompletableFuture<Void> terminal) {}
}
